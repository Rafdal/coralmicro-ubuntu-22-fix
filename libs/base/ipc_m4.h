#ifndef LIBS_BASE_IPC_M4_H_
#define LIBS_BASE_IPC_M4_H_

#include <functional>

#include "libs/base/ipc.h"

namespace coral::micro {
class IPCM4 : public IPC {
   public:
    static IPCM4* GetSingleton() {
        static IPCM4 ipc;
        return &ipc;
    }

    void Init() override;

   protected:
    void RxTaskFn() override;

   private:
    void HandleSystemMessage(const ipc::SystemMessage& message) override;
};
}  // namespace coral::micro

#endif  // LIBS_BASE_IPC_M4_H_
