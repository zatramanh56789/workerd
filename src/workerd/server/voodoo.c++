// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This server interacts directly with the GPU, and listens on a UNIX socket for clients
// of the Dawn Wire protocol.

#include "voodoo-pipe.h"
#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>
#include <dawn/wire/WireServer.h>
#include <filesystem>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/main.h>
#include <unistd.h>

// dawn buffer sizes
#define DAWNCMD_MAX (4096 * 32)

struct DawnRemoteSerializer : public dawn::wire::CommandSerializer {
  void* GetCmdSpace(size_t size) override {
    KJ_UNIMPLEMENTED();
  }
  bool Flush() override {
    KJ_UNIMPLEMENTED();
  };
  size_t GetMaximumAllocationSize() const override {
    KJ_DBG("GetMaximumAllocationSize() was called");
    return DAWNCMD_MAX;
  };
};

class VoodooMain : public kj::TaskSet::ErrorHandler {
public:
  VoodooMain(kj::ProcessContext& context)
      : context(context), nativeProcs(dawn::native::GetProcs()) {}

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "task failed handling connection", exception);
  }

  kj::MainBuilder::Validity setListenPath(kj::StringPtr path) {
    listenPath = path;
    return true;
  }

  kj::MainBuilder::Validity startServer() {
    KJ_DBG(listenPath, "will start listening server");

    // initialize dawn
    dawnProcSetProcs(&nativeProcs);
    auto adapters = instance.EnumerateAdapters();
    KJ_REQUIRE(!adapters.empty(), "no GPU adapters found");

    // initialize event loop
    kj::AsyncIoContext io = kj::setupAsyncIo();

    // create listening socket
    unlink(listenPath.cStr());
    auto addr =
        io.provider->getNetwork().parseAddress(kj::str("unix:", listenPath)).wait(io.waitScope);

    auto listener = addr->listen();

    // process requests
    auto promise = acceptLoop(kj::mv(listener));
    promise.wait(io.waitScope);
    return true;
  }

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Voodoo GPU handler V0.0",
                           "Exposes a Dawn Wire endpoint on a UNIX socket for dawn clients that "
                           "want to interact with a GPU")
        .expectArg("<listen_path>", KJ_BIND_METHOD(*this, setListenPath))
        .callAfterParsing(KJ_BIND_METHOD(*this, startServer))
        .build();
  }

  kj::Promise<void> acceptLoop(kj::Own<kj::ConnectionReceiver>&& listener) {
    kj::TaskSet tasks(*this);

    for (;;) {
      auto connection = co_await listener->accept();
      tasks.add(handleConnection(kj::mv(connection)));
    }
  }

  kj::Promise<void> handleConnection(kj::Own<kj::AsyncIoStream> stream) {
    co_await kj::Promise<void>(kj::READY_NOW);
    KJ_DBG("handling connection");

    // setup wire
    auto serializer = kj::heap<DawnRemoteSerializer>();
    dawn::wire::WireServerDescriptor wDesc{
        .serializer = serializer,
        .procs = &nativeProcs,
    };

    auto wireServer = kj::heap<dawn::wire::WireServer>(wDesc);
    wireServer->InjectInstance(instance.Get(), 1, 0);

    Pipe<4096> _wbuf;
    auto c = _wbuf.cap();
    KJ_DBG(c, stream);
    auto t = co_await _wbuf.readFromStream(stream, 10);
    KJ_DBG("all done");
    KJ_UNIMPLEMENTED();
  }

private:
  kj::StringPtr listenPath;
  kj::ProcessContext& context;
  DawnProcTable nativeProcs;
  dawn::native::Instance instance;
};

KJ_MAIN(VoodooMain)
