// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libs/base/filesystem.h"
#include "libs/camera/camera.h"
#include "libs/rpc/rpc_http_server.h"
#include "libs/tensorflow/posenet.h"
#include "libs/tensorflow/posenet_decoder_op.h"
#include "libs/tpu/edgetpu_manager.h"
#include "third_party/freertos_kernel/include/FreeRTOS.h"
#include "third_party/freertos_kernel/include/task.h"
#include "third_party/mjson/src/mjson.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/all_ops_resolver.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_error_reporter.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_interpreter.h"
#include "third_party/tflite-micro/tensorflow/lite/micro/micro_mutable_op_resolver.h"

namespace coralmicro {
namespace {
constexpr int kTensorArenaSize = 2 * 1024 * 1024;
constexpr float kConfidenceThreshold = 0.5f;
STATIC_TENSOR_ARENA_IN_SDRAM(tensor_arena, kTensorArenaSize);
constexpr char kModelPath[] =
    "/models/bodypix_mobilenet_v1_075_324_324_16_quant_decoder_edgetpu.tflite";

void RunBodypix(struct jsonrpc_request* r) {
  auto* interpreter =
      reinterpret_cast<tflite::MicroInterpreter*>(r->ctx->response_cb_data);
  auto* bodypix_input = interpreter->input(0);
  auto model_height = bodypix_input->dims->data[1];
  auto model_width = bodypix_input->dims->data[2];

  // Starts the camera for live poses.
  CameraTask::GetSingleton()->SetPower(true);
  CameraTask::GetSingleton()->Enable(CameraMode::kStreaming);

  // Loop until a result is found
  printf(
      "Running Bodypix until a result with greater than %f confidence is "
      "found\r\n",
      kConfidenceThreshold);
  std::vector<uint8_t> image(model_width * model_height *
                             CameraFormatBpp(CameraFormat::kRgb));
  std::vector<tensorflow::Pose> results;

  CameraFrameFormat fmt{/*fmt=*/CameraFormat::kRgb,
                        /*filter=*/CameraFilterMethod::kBilinear,
                        /*rotation=*/CameraRotation::k0,
                        /*width=*/model_width,
                        /*height=*/model_height,
                        /*preserve_ratio=*/false,
                        /*buffer=*/image.data()};

  // Discard the first frame to ensure no power-on artifacts exist.
  CameraTask::GetSingleton()->GetFrame({fmt});

  for (;;) {
    if (!CameraTask::GetSingleton()->GetFrame({fmt})) {
      jsonrpc_return_error(r, -1, "Failed to get image from camera.", nullptr);
      return;
    }

    std::memcpy(tflite::GetTensorData<uint8_t>(bodypix_input), image.data(),
                image.size());

    if (interpreter->Invoke() != kTfLiteOk) {
      jsonrpc_return_error(r, -1, "Invoke failed", nullptr);
      return;
    }

    results = tensorflow::GetPosenetOutput(interpreter, kConfidenceThreshold);
    if (results.size() > 0) {
      break;
    }
  }

  CameraTask::GetSingleton()->Disable();
  CameraTask::GetSingleton()->SetPower(false);

  const auto& float_segments_tensor = interpreter->output_tensor(5);
  const auto& float_segments =
      tflite::GetTensorData<uint8_t>(float_segments_tensor);
  const auto segment_size = tensorflow::TensorSize(float_segments_tensor);

  jsonrpc_return_success(r, "{%Q: %d, %Q: %d, %Q: %V, %Q: %V}", "width",
                         model_width, "height", model_height, "base64_data",
                         image.size(), image.data(), "output_mask1",
                         segment_size, float_segments);

  return;
}

void Main() {
  tflite::MicroErrorReporter error_reporter;
  printf("Bodypix!\r\n");

  // Turn on the TPU and get it's context.
  auto tpu_context =
      EdgeTpuManager::GetSingleton()->OpenDevice(PerformanceMode::kMax);
  if (!tpu_context) {
    TF_LITE_REPORT_ERROR(&error_reporter,
                         "ERROR: Failed to get EdgeTpu context!");
    vTaskSuspend(nullptr);
  }

  // Reads the model and checks version.
  std::vector<uint8_t> bodypix_tflite;
  if (!LfsReadFile(kModelPath, &bodypix_tflite)) {
    TF_LITE_REPORT_ERROR(&error_reporter, "Failed to load model!");
    vTaskSuspend(nullptr);
  }
  auto* model = tflite::GetModel(bodypix_tflite.data());
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    TF_LITE_REPORT_ERROR(&error_reporter,
                         "Model schema version is %d, supported is %d",
                         model->version(), TFLITE_SCHEMA_VERSION);
    vTaskSuspend(nullptr);
  }

  // Creates a micro interpreter.
  tflite::MicroMutableOpResolver<2> resolver;
  resolver.AddCustom(kCustomOp, RegisterCustomOp());
  resolver.AddCustom(kPosenetDecoderOp, RegisterPosenetDecoderOp());
  tflite::MicroInterpreter interpreter = tflite::MicroInterpreter{
      model, resolver, tensor_arena, kTensorArenaSize, &error_reporter};
  if (interpreter.AllocateTensors() != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(&error_reporter, "AllocateTensors failed.");
    vTaskSuspend(nullptr);
  }

  printf("Initializing Bodypix server...%p\r\n", &interpreter);
  jsonrpc_init(nullptr, &interpreter);
  jsonrpc_export("run_bodypix", RunBodypix);
  UseHttpServer(new JsonRpcHttpServer);
  printf("Bodypix server ready!\r\n");
  vTaskSuspend(nullptr);
}
}  // namespace
}  // namespace coralmicro

extern "C" void app_main(void* param) {
  (void)param;
  coralmicro::Main();
  vTaskSuspend(nullptr);
}