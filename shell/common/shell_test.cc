// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/shell/common/shell_test.h"

#include "flutter/flow/frame_timings.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/fml/build_config.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/mapping.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/shell_test_platform_view.h"
#include "flutter/shell/common/vsync_waiter_fallback.h"
#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {

ShellTest::ShellTest()
    : thread_host_("io.flutter.test." + GetCurrentTestName() + ".",
                   ThreadHost::Type::Platform | ThreadHost::Type::IO |
                       ThreadHost::Type::UI | ThreadHost::Type::RASTER) {}

void ShellTest::SendEnginePlatformMessage(
    Shell* shell,
    std::unique_ptr<PlatformMessage> message) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      fml::MakeCopyable(
          [shell, &latch, message = std::move(message)]() mutable {
            if (auto engine = shell->weak_engine_) {
              engine->HandlePlatformMessage(std::move(message));
            }
            latch.Signal();
          }));
  latch.Wait();
}

void ShellTest::PlatformViewNotifyCreated(Shell* shell) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
        shell->GetPlatformView()->NotifyCreated();
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::PlatformViewNotifyDestroyed(Shell* shell) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
        shell->GetPlatformView()->NotifyDestroyed();
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::RunEngine(Shell* shell, RunConfiguration configuration) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [shell, &latch, &configuration]() {
        shell->RunEngine(std::move(configuration),
                         [&latch](Engine::RunStatus run_status) {
                           ASSERT_EQ(run_status, Engine::RunStatus::Success);
                           latch.Signal();
                         });
      });
  latch.Wait();
}

void ShellTest::RestartEngine(Shell* shell, RunConfiguration configuration) {
  std::promise<bool> restarted;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetUITaskRunner(),
      [shell, &restarted, &configuration]() {
        restarted.set_value(shell->engine_->Restart(std::move(configuration)));
      });
  ASSERT_TRUE(restarted.get_future().get());
}

void ShellTest::VSyncFlush(Shell* shell, bool& will_draw_new_frame) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [shell, &will_draw_new_frame, &latch] {
        // The following UI task ensures that all previous UI tasks are flushed.
        fml::AutoResetWaitableEvent ui_latch;
        shell->GetTaskRunners().GetUITaskRunner()->PostTask(
            [&ui_latch, &will_draw_new_frame]() {
              will_draw_new_frame = true;
              ui_latch.Signal();
            });

        ShellTestPlatformView* test_platform_view =
            static_cast<ShellTestPlatformView*>(shell->GetPlatformView().get());
        do {
          test_platform_view->SimulateVSync();
        } while (ui_latch.WaitWithTimeout(fml::TimeDelta::FromMilliseconds(1)));

        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::SetViewportMetrics(Shell* shell, double width, double height) {
  flutter::ViewportMetrics viewport_metrics = {
      1,                      // device pixel ratio
      width,                  // physical width
      height,                 // physical height
      0,                      // padding top
      0,                      // padding right
      0,                      // padding bottom
      0,                      // padding left
      0,                      // view inset top
      0,                      // view inset right
      0,                      // view inset bottom
      0,                      // view inset left
      0,                      // gesture inset top
      0,                      // gesture inset right
      0,                      // gesture inset bottom
      0,                      // gesture inset left
      22,                     // physical touch slop
      std::vector<double>(),  // display features bounds
      std::vector<int>(),     // display features type
      std::vector<int>(),     // display features state
      0                       // Display ID
  };
  // Set viewport to nonempty, and call Animator::BeginFrame to make the layer
  // tree pipeline nonempty. Without either of this, the layer tree below
  // won't be rasterized.
  fml::AutoResetWaitableEvent latch;
  shell->GetTaskRunners().GetUITaskRunner()->PostTask(
      [&latch, engine = shell->weak_engine_, viewport_metrics]() {
        if (engine) {
          engine->SetViewportMetrics(viewport_metrics);
          const auto frame_begin_time = fml::TimePoint::Now();
          const auto frame_end_time =
              frame_begin_time + fml::TimeDelta::FromSecondsF(1.0 / 60.0);
          std::unique_ptr<FrameTimingsRecorder> recorder =
              std::make_unique<FrameTimingsRecorder>();
          recorder->RecordVsync(frame_begin_time, frame_end_time);
          engine->animator_->BeginFrame(std::move(recorder));
        }
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::NotifyIdle(Shell* shell, fml::TimeDelta deadline) {
  fml::AutoResetWaitableEvent latch;
  shell->GetTaskRunners().GetUITaskRunner()->PostTask(
      [&latch, engine = shell->weak_engine_, deadline]() {
        if (engine) {
          engine->NotifyIdle(deadline);
        }
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::PumpOneFrame(Shell* shell,
                             double width,
                             double height,
                             LayerTreeBuilder builder) {
  PumpOneFrame(shell, {1.0, width, height, 22, 0}, std::move(builder));
}

void ShellTest::PumpOneFrame(Shell* shell,
                             const flutter::ViewportMetrics& viewport_metrics,
                             LayerTreeBuilder builder) {
  // Set viewport to nonempty, and call Animator::BeginFrame to make the layer
  // tree pipeline nonempty. Without either of this, the layer tree below
  // won't be rasterized.
  fml::AutoResetWaitableEvent latch;
  shell->GetTaskRunners().GetUITaskRunner()->PostTask(
      [&latch, engine = shell->weak_engine_, viewport_metrics]() {
        engine->SetViewportMetrics(viewport_metrics);
        const auto frame_begin_time = fml::TimePoint::Now();
        const auto frame_end_time =
            frame_begin_time + fml::TimeDelta::FromSecondsF(1.0 / 60.0);
        std::unique_ptr<FrameTimingsRecorder> recorder =
            std::make_unique<FrameTimingsRecorder>();
        recorder->RecordVsync(frame_begin_time, frame_end_time);
        engine->animator_->BeginFrame(std::move(recorder));
        latch.Signal();
      });
  latch.Wait();

  latch.Reset();
  // Call |Render| to rasterize a layer tree and trigger |OnFrameRasterized|
  fml::WeakPtr<RuntimeDelegate> runtime_delegate = shell->weak_engine_;
  shell->GetTaskRunners().GetUITaskRunner()->PostTask(
      [&latch, runtime_delegate, &builder, viewport_metrics]() {
        SkMatrix identity;
        identity.setIdentity();
        auto root_layer = std::make_shared<TransformLayer>(identity);
        auto layer_tree = std::make_unique<LayerTree>(
            LayerTree::Config{.root_layer = root_layer},
            SkISize::Make(viewport_metrics.physical_width,
                          viewport_metrics.physical_height));
        float device_pixel_ratio =
            static_cast<float>(viewport_metrics.device_pixel_ratio);
        if (builder) {
          builder(root_layer);
        }
        runtime_delegate->Render(std::move(layer_tree), device_pixel_ratio);
        latch.Signal();
      });
  latch.Wait();
}

void ShellTest::DispatchFakePointerData(Shell* shell) {
  auto packet = std::make_unique<PointerDataPacket>(1);
  DispatchPointerData(shell, std::move(packet));
}

void ShellTest::DispatchPointerData(Shell* shell,
                                    std::unique_ptr<PointerDataPacket> packet) {
  fml::AutoResetWaitableEvent latch;
  shell->GetTaskRunners().GetPlatformTaskRunner()->PostTask(
      [&latch, shell, &packet]() {
        // Goes through PlatformView to ensure packet is corrected converted.
        shell->GetPlatformView()->DispatchPointerDataPacket(std::move(packet));
        latch.Signal();
      });
  latch.Wait();
}

int ShellTest::UnreportedTimingsCount(Shell* shell) {
  return shell->unreported_timings_.size();
}

void ShellTest::SetNeedsReportTimings(Shell* shell, bool value) {
  shell->SetNeedsReportTimings(value);
}

bool ShellTest::GetNeedsReportTimings(Shell* shell) {
  return shell->needs_report_timings_;
}

void ShellTest::StorePersistentCache(PersistentCache* cache,
                                     const SkData& key,
                                     const SkData& value) {
  cache->store(key, value);
}

void ShellTest::OnServiceProtocol(
    Shell* shell,
    ServiceProtocolEnum some_protocol,
    const fml::RefPtr<fml::TaskRunner>& task_runner,
    const ServiceProtocol::Handler::ServiceProtocolMap& params,
    rapidjson::Document* response) {
  std::promise<bool> finished;
  fml::TaskRunner::RunNowOrPostTask(task_runner, [shell, some_protocol, params,
                                                  response, &finished]() {
    switch (some_protocol) {
      case ServiceProtocolEnum::kGetSkSLs:
        shell->OnServiceProtocolGetSkSLs(params, response);
        break;
      case ServiceProtocolEnum::kEstimateRasterCacheMemory:
        shell->OnServiceProtocolEstimateRasterCacheMemory(params, response);
        break;
      case ServiceProtocolEnum::kSetAssetBundlePath:
        shell->OnServiceProtocolSetAssetBundlePath(params, response);
        break;
      case ServiceProtocolEnum::kRunInView:
        shell->OnServiceProtocolRunInView(params, response);
        break;
      case ServiceProtocolEnum::kRenderFrameWithRasterStats:
        shell->OnServiceProtocolRenderFrameWithRasterStats(params, response);
        break;
    }
    finished.set_value(true);
  });
  finished.get_future().wait();
}

std::shared_ptr<txt::FontCollection> ShellTest::GetFontCollection(
    Shell* shell) {
  return shell->weak_engine_->GetFontCollection().GetFontCollection();
}

Settings ShellTest::CreateSettingsForFixture() {
  Settings settings;
  settings.leak_vm = false;
  settings.task_observer_add = [](intptr_t key, const fml::closure& handler) {
    fml::MessageLoop::GetCurrent().AddTaskObserver(key, handler);
  };
  settings.task_observer_remove = [](intptr_t key) {
    fml::MessageLoop::GetCurrent().RemoveTaskObserver(key);
  };
  settings.isolate_create_callback = [this]() {
    native_resolver_->SetNativeResolverForIsolate();
  };
#if OS_FUCHSIA
  settings.verbose_logging = true;
#endif
  SetSnapshotsAndAssets(settings);
  return settings;
}

TaskRunners ShellTest::GetTaskRunnersForFixture() {
  return {
      "test",
      thread_host_.platform_thread->GetTaskRunner(),  // platform
      thread_host_.raster_thread->GetTaskRunner(),    // raster
      thread_host_.ui_thread->GetTaskRunner(),        // ui
      thread_host_.io_thread->GetTaskRunner()         // io
  };
}

fml::TimePoint ShellTest::GetLatestFrameTargetTime(Shell* shell) const {
  return shell->GetLatestFrameTargetTime();
}

std::unique_ptr<Shell> ShellTest::CreateShell(const Settings& settings,
                                              bool simulate_vsync) {
  return CreateShell(settings, GetTaskRunnersForFixture(), simulate_vsync);
}

std::unique_ptr<Shell> ShellTest::CreateShell(
    const Settings& settings,
    TaskRunners task_runners,
    bool simulate_vsync,
    const std::shared_ptr<ShellTestExternalViewEmbedder>&
        shell_test_external_view_embedder,
    bool is_gpu_disabled,
    ShellTestPlatformView::BackendType rendering_backend,
    Shell::CreateCallback<PlatformView> platform_view_create_callback) {
  const auto vsync_clock = std::make_shared<ShellTestVsyncClock>();

  CreateVsyncWaiter create_vsync_waiter = [&]() {
    if (simulate_vsync) {
      return static_cast<std::unique_ptr<VsyncWaiter>>(
          std::make_unique<ShellTestVsyncWaiter>(task_runners, vsync_clock));
    } else {
      return static_cast<std::unique_ptr<VsyncWaiter>>(
          std::make_unique<VsyncWaiterFallback>(task_runners, true));
    }
  };

  if (!platform_view_create_callback) {
    platform_view_create_callback = [vsync_clock,                        //
                                     &create_vsync_waiter,               //
                                     shell_test_external_view_embedder,  //
                                     rendering_backend                   //
    ](Shell& shell) {
      return ShellTestPlatformView::Create(
          shell,                                  //
          shell.GetTaskRunners(),                 //
          vsync_clock,                            //
          create_vsync_waiter,                    //
          rendering_backend,                      //
          shell_test_external_view_embedder,      //
          shell.GetConcurrentWorkerTaskRunner(),  //
          shell.GetIsGpuDisabledSyncSwitch()      //
      );
    };
  }

  Shell::CreateCallback<Rasterizer> rasterizer_create_callback =
      [](Shell& shell) { return std::make_unique<Rasterizer>(shell); };

  return Shell::Create(flutter::PlatformData(),        //
                       task_runners,                   //
                       settings,                       //
                       platform_view_create_callback,  //
                       rasterizer_create_callback,     //
                       is_gpu_disabled                 //
  );
}

void ShellTest::DestroyShell(std::unique_ptr<Shell> shell) {
  DestroyShell(std::move(shell), GetTaskRunnersForFixture());
}

void ShellTest::DestroyShell(std::unique_ptr<Shell> shell,
                             const TaskRunners& task_runners) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(task_runners.GetPlatformTaskRunner(),
                                    [&shell, &latch]() mutable {
                                      shell.reset();
                                      latch.Signal();
                                    });
  latch.Wait();
}

size_t ShellTest::GetLiveTrackedPathCount(
    const std::shared_ptr<VolatilePathTracker>& tracker) {
  return std::count_if(
      tracker->paths_.begin(), tracker->paths_.end(),
      [](const std::weak_ptr<VolatilePathTracker::TrackedPath>& path) {
        return path.lock();
      });
}

}  // namespace testing
}  // namespace flutter
