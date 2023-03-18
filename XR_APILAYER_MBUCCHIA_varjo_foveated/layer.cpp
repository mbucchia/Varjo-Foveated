// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

namespace {

    using namespace openxr_api_layer;
    using namespace openxr_api_layer::log;

    class OpenXrLayer : public openxr_api_layer::OpenXrApi {
      public:
        OpenXrLayer() = default;

        // Corresponds to xrDestroyInstance().
        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        ~OpenXrLayer() = default;

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetInstanceProcAddr
        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetInstanceProcAddr",
                              TLXArg(instance, "Instance"),
                              TLArg(name, "Name"),
                              TLArg(m_bypassApiLayer, "Bypass"));

            XrResult result = m_bypassApiLayer ? m_xrGetInstanceProcAddr(instance, name, function)
                                               : OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLPArg(*function, "Function"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            // See if the instance supports quad views to begin with.
            m_bypassApiLayer = std::find(GetGrantedExtensions().cbegin(),
                                         GetGrantedExtensions().cend(),
                                         XR_VARJO_QUAD_VIEWS_EXTENSION_NAME) == GetGrantedExtensions().cend();
            if (m_bypassApiLayer) {
                Log(fmt::format("{} layer will be bypassed\n", LayerName));
                return XR_SUCCESS;
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name and OpenXR runtime information to help debugging issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log(fmt::format("Using OpenXR runtime: {}\n", runtimeName));

            // Check for system capabilities.
            XrSystemId systemId;
            XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
            systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            CHECK_XRCMD(OpenXrApi::xrGetSystem(GetXrInstance(), &systemInfo, &systemId));
            XrSystemFoveatedRenderingPropertiesVARJO foveatedRenderingProperties{
                XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO};
            XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES, &foveatedRenderingProperties};
            CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(GetXrInstance(), systemId, &systemProperties));
            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystem",
                TLArg(systemProperties.systemName, "SystemName"),
                TLArg(foveatedRenderingProperties.supportsFoveatedRendering, "SupportsFoveatedRendering"));
            Log(fmt::format("Using OpenXR system: {}\n", systemProperties.systemName));
            Log(fmt::format("supportsFoveatedRendering = {}\n", foveatedRenderingProperties.supportsFoveatedRendering));

            LoadConfiguration();

            return XR_SUCCESS;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews
        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurationViews",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            // Insert the foveated configuration flag if needed.
            std::vector<XrFoveatedViewConfigurationViewVARJO> foveatedView(
                4, {XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO});
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                for (uint32_t i = 0; i < viewCapacityInput; i++) {
                    foveatedView[i].foveatedRenderingActive = !m_noEyeTracking;
                    foveatedView[i].next = views[i].next;
                    views[i].next = &foveatedView[i];
                }
            }

            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(
                    g_traceProvider, "xrEnumerateViewConfigurationViews", TLArg(*viewCountOutput, "ViewCountOutput"));

                if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                    if (viewCapacityInput) {
#pragma warning(push)
#pragma warning(disable : 4244)
                        views[0].recommendedImageRectWidth *= m_peripheralResolutionFactor;
                        views[0].recommendedImageRectHeight *= m_peripheralResolutionFactor;
                        views[1].recommendedImageRectWidth *= m_peripheralResolutionFactor;
                        views[1].recommendedImageRectHeight *= m_peripheralResolutionFactor;
                        views[2].recommendedImageRectWidth *= m_focusResolutionFactor;
                        views[2].recommendedImageRectHeight *= m_focusResolutionFactor;
                        views[3].recommendedImageRectWidth *= m_focusResolutionFactor;
                        views[3].recommendedImageRectHeight *= m_focusResolutionFactor;
#pragma warning(pop)

                        Log(fmt::format("Peripheral resolution: {}x{} (multiplier: {:.3f})\n",
                                        views[0].recommendedImageRectWidth,
                                        views[0].recommendedImageRectHeight,
                                        m_peripheralResolutionFactor));
                        Log(fmt::format("Focus resolution {}x{} (multiplier: {:.3f})\n",
                                        views[2].recommendedImageRectWidth,
                                        views[2].recommendedImageRectHeight,
                                        m_focusResolutionFactor));
                    }

                    for (uint32_t i = 0; i < viewCapacityInput; i++) {
                        // Propagate the maximum.
                        views[i].maxImageRectWidth =
                            std::max(views[i].maxImageRectWidth, views[i].recommendedImageRectWidth);
                        views[i].maxImageRectHeight =
                            std::max(views[i].maxImageRectHeight, views[i].recommendedImageRectHeight);

                        TraceLoggingWrite(
                            g_traceProvider,
                            "xrEnumerateViewConfigurationViews",
                            TLArg(views[i].maxImageRectWidth, "MaxImageRectWidth"),
                            TLArg(views[i].maxImageRectHeight, "MaxImageRectHeight"),
                            TLArg(views[i].maxSwapchainSampleCount, "MaxSwapchainSampleCount"),
                            TLArg(views[i].recommendedImageRectWidth, "RecommendedImageRectWidth"),
                            TLArg(views[i].recommendedImageRectHeight, "RecommendedImageRectHeight"),
                            TLArg(views[i].recommendedSwapchainSampleCount, "RecommendedSwapchainSampleCount"));
                    }
                }
            }

            // Undo our changes to the app structs.
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                for (uint32_t i = 0; i < viewCapacityInput; i++) {
                    views[i].next = foveatedView[i].next;
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain
        XrResult xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
            if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLXArg(session, "Session"),
                              TLArg(createInfo->arraySize, "ArraySize"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->createFlags, "CreateFlags"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->faceCount, "FaceCount"),
                              TLArg(createInfo->mipCount, "MipCount"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->usageFlags, "UsageFlags"));
            Log(fmt::format("Creating swapchain with resolution: {}x{}\n", createInfo->width, createInfo->height));

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLXArg(*swapchain, "Swapchain"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain
        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLXArg(swapchain, "Session"));

            // In Turbo Mode, make sure there is no pending frame that may potentially hold onto the swapchain.
            {
                std::unique_lock lock(m_frameLock);

                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    m_asyncWaitPromise.wait();
                    TraceLoggingWriteStop(local, "AsyncWaitNow");
                }
            }

            return OpenXrApi::xrDestroySwapchain(swapchain);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginSession
        XrResult xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) override {
            if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(
                g_traceProvider,
                "xrBeginSession",
                TLXArg(session, "Session"),
                TLArg(xr::ToCString(beginInfo->primaryViewConfigurationType), "PrimaryViewConfigurationType"));

            const XrResult result = OpenXrApi::xrBeginSession(session, beginInfo);

            m_initialized = false;

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));

            // Wait for deferred frames to finish before teardown.
            if (m_asyncWaitPromise.valid()) {
                TraceLocalActivity(local);

                TraceLoggingWriteStart(local, "AsyncWaitNow");
                m_asyncWaitPromise.wait_for(5s);
                TraceLoggingWriteStop(local, "AsyncWaitNow");

                m_asyncWaitPromise = {};
            }

            return OpenXrApi::xrDestroySession(session);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews
        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLXArg(session, "Session"),
                              TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                              TLXArg(viewLocateInfo->space, "Space"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            // Insert the foveated location flag if needed.
            XrViewLocateFoveatedRenderingVARJO viewLocateFoveatedRendering{
                XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO};
            if (viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                bool foveationActive = false;
                if (!m_noEyeTracking) {
                    {
                        std::unique_lock lock(m_resourcesMutex);

                        if (!m_initialized) {
                            XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                            spaceInfo.poseInReferenceSpace = xr::math::Pose::Identity();
                            CHECK_XRCMD(xrCreateReferenceSpace(session, &spaceInfo, &m_viewSpace));

                            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
                            spaceInfo.poseInReferenceSpace = xr::math::Pose::Identity();
                            CHECK_XRCMD(xrCreateReferenceSpace(session, &spaceInfo, &m_renderGazeSpace));

                            m_initialized = true;
                        }
                    }

                    XrSpaceLocation renderGazeLocation{XR_TYPE_SPACE_LOCATION};
                    CHECK_XRCMD(xrLocateSpace(
                        m_renderGazeSpace, m_viewSpace, viewLocateInfo->displayTime, &renderGazeLocation));
                    foveationActive =
                        (renderGazeLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;

                    viewLocateFoveatedRendering.foveatedRenderingActive = foveationActive;
                }

                TraceLoggingWrite(g_traceProvider, "xrLocateViews", TLArg(foveationActive, "FoveationActive"));

                viewLocateFoveatedRendering.next = viewLocateInfo->next;
                const_cast<XrViewLocateInfo*>(viewLocateInfo)->next = &viewLocateFoveatedRendering;
            }

            const XrResult result =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrLocateViews",
                                  TLArg(*viewCountOutput, "ViewCountOutput"),
                                  TLArg(viewState->viewStateFlags, "ViewStateFlags"));

                for (uint32_t i = 0; i < *viewCountOutput; i++) {
                    TraceLoggingWrite(g_traceProvider,
                                      "xrLocateViews",
                                      TLArg(xr::ToString(views[i].pose).c_str(), "Pose"),
                                      TLArg(xr::ToString(views[i].fov).c_str(), "Fov"));
                }
            }

            // Undo our changes to the app structs.
            if (viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                const_cast<XrViewLocateInfo*>(viewLocateInfo)->next = viewLocateFoveatedRendering.next;
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrAcquireSwapchainImage
        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLXArg(swapchain, "Swapchain"));

            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLArg(*index, "Index"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrWaitSwapchainImage
        XrResult xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) override {
            if (waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrWaitSwapchainImage",
                              TLXArg(swapchain, "Swapchain"),
                              TLArg(waitInfo->timeout, "Timeout"));

            return OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrReleaseSwapchainImage
        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLXArg(swapchain, "Swapchain"));

            return OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrWaitFrame
        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            TraceLoggingWrite(g_traceProvider, "xrWaitFrame", TLXArg(session, "Session"));

            const auto lastFrameWaitTimestamp = m_lastFrameWaitTimestamp;
            m_lastFrameWaitTimestamp = std::chrono::steady_clock::now();

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            {
                std::unique_lock lock(m_frameLock);

                if (m_asyncWaitPromise.valid()) {
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");

                    // In Turbo mode, we accept pipelining of exactly one frame.
                    if (m_asyncWaitPolled) {
                        TraceLocalActivity(local);

                        // On second frame poll, we must wait.
                        TraceLoggingWriteStart(local, "AsyncWaitNow");
                        m_asyncWaitPromise.wait();
                        TraceLoggingWriteStop(local, "AsyncWaitNow");
                    }
                    m_asyncWaitPolled = true;

                    // In Turbo mode, we don't actually wait, we make up a predicted time.
                    {
                        std::unique_lock lock(m_asyncWaitLock);

                        frameState->predictedDisplayTime =
                            m_asyncWaitCompleted ? m_lastPredictedDisplayTime
                                                 : (m_lastPredictedDisplayTime +
                                                    (m_lastFrameWaitTimestamp - lastFrameWaitTimestamp).count());
                        frameState->predictedDisplayPeriod = m_lastPredictedDisplayPeriod;
                    }
                    frameState->shouldRender = XR_TRUE;

                    result = XR_SUCCESS;

                } else {
                    lock.unlock();
                    result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
                    lock.lock();

                    if (XR_SUCCEEDED(result)) {
                        // We must always store those values to properly handle transitions into Turbo Mode.
                        m_lastPredictedDisplayTime = frameState->predictedDisplayTime;
                        m_lastPredictedDisplayPeriod = frameState->predictedDisplayPeriod;
                    }
                }
            }

            if (XR_SUCCEEDED(result)) {
                // Per OpenXR spec, the predicted display must increase monotonically.
                frameState->predictedDisplayTime = std::max(frameState->predictedDisplayTime, m_waitedFrameTime + 1);

                // Record the predicted display time.
                m_waitedFrameTime = frameState->predictedDisplayTime;

                TraceLoggingWrite(g_traceProvider,
                                  "xrWaitFrame",
                                  TLArg(!!frameState->shouldRender, "ShouldRender"),
                                  TLArg(frameState->predictedDisplayTime, "PredictedDisplayTime"),
                                  TLArg(frameState->predictedDisplayPeriod, "PredictedDisplayPeriod"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            TraceLoggingWrite(g_traceProvider, "xrBeginFrame", TLXArg(session, "Session"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            {
                std::unique_lock lock(m_frameLock);

                if (m_asyncWaitPromise.valid()) {
                    // In turbo mode, we do nothing here.
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");
                    result = XR_SUCCESS;
                } else {
                    result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLXArg(session, "Session"),
                              TLArg(frameEndInfo->displayTime, "DisplayTime"),
                              TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"),
                              TLArg(frameEndInfo->layerCount, "LayerCount"));

            for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
                if (!frameEndInfo->layers[i]) {
                    return XR_ERROR_LAYER_INVALID;
                }

                if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const XrCompositionLayerProjection* proj =
                        reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame_Layer",
                                      TLArg("Projection", "Type"),
                                      TLArg(proj->layerFlags, "Flags"),
                                      TLXArg(proj->space, "Space"),
                                      TLArg(proj->viewCount, "ViewCount"));

                    for (uint32_t eye = 0; eye < proj->viewCount; eye++) {
                        TraceLoggingWrite(g_traceProvider,
                                          "xrEndFrame_View",
                                          TLArg("Projection", "Type"),
                                          TLArg(eye, "Index"),
                                          TLXArg(proj->views[eye].subImage.swapchain, "Swapchain"),
                                          TLArg(proj->views[eye].subImage.imageArrayIndex, "ImageArrayIndex"),
                                          TLArg(xr::ToString(proj->views[eye].subImage.imageRect).c_str(), "ImageRect"),
                                          TLArg(xr::ToString(proj->views[eye].pose).c_str(), "Pose"),
                                          TLArg(xr::ToString(proj->views[eye].fov).c_str(), "Fov"));
                    }
                }
            }

            XrResult result;
            {
                std::unique_lock lock(m_frameLock);

                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    // This is the latest point we must have fully waited a frame before proceeding.
                    //
                    // Note: we should not wait infinitely here, however certain patterns of engine calls may cause us
                    // to attempt a "double xrWaitFrame" when turning on Turbo. Use a timeout to detect that, and
                    // refrain from enqueing a second wait further down. This isn't a pretty solution, but it is simple
                    // and it seems to work effectively (minus the 1s freeze observed in-game).
                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    const auto ready = m_asyncWaitPromise.wait_for(1s) == std::future_status::ready;
                    TraceLoggingWriteStop(local, "AsyncWaitNow", TLArg(ready, "Ready"));
                    if (ready) {
                        m_asyncWaitPromise = {};
                    }

                    CHECK_XRCMD(OpenXrApi::xrBeginFrame(session, nullptr));
                }

                result = OpenXrApi::xrEndFrame(session, frameEndInfo);

                if (m_useTurboMode && !m_asyncWaitPromise.valid()) {
                    m_asyncWaitPolled = false;
                    m_asyncWaitCompleted = false;

                    // In Turbo mode, we kick off a wait thread immediately.
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitStart");
                    m_asyncWaitPromise = std::async(std::launch::async, [&, session] {
                        TraceLocalActivity(local);

                        XrFrameState frameState{XR_TYPE_FRAME_STATE};
                        TraceLoggingWriteStart(local, "AsyncWaitFrame");
                        CHECK_XRCMD(OpenXrApi::xrWaitFrame(session, nullptr, &frameState));
                        TraceLoggingWriteStop(local,
                                              "AsyncWaitFrame",
                                              TLArg(frameState.predictedDisplayTime, "PredictedDisplayTime"),
                                              TLArg(frameState.predictedDisplayPeriod, "PredictedDisplayPeriod"));
                        {
                            std::unique_lock lock(m_asyncWaitLock);

                            m_lastPredictedDisplayTime = frameState.predictedDisplayTime;
                            m_lastPredictedDisplayPeriod = frameState.predictedDisplayPeriod;

                            m_asyncWaitCompleted = true;
                        }
                    });
                }
            }

            return result;
        }

      private:
        void LoadConfiguration() {
            std::ifstream configFile;

            // Look in %LocalAppData% first, then fallback to your installation folder.
            configFile.open(localAppData / (LayerName + ".cfg"));
            if (!configFile.is_open()) {
                configFile.open(dllHome / (LayerName + ".cfg"));
            }

            if (configFile.is_open()) {
                unsigned int lineNumber = 0;
                std::string line;
                while (std::getline(configFile, line)) {
                    lineNumber++;
                    ParseConfigurationStatement(line, lineNumber);
                }
                configFile.close();
            } else {
                Log("No configuration was found\n");
            }
        }

        void ParseConfigurationStatement(const std::string& line, unsigned int lineNumber) {
            try {
                const auto offset = line.find('=');
                if (offset != std::string::npos) {
                    const std::string name = line.substr(0, offset);
                    const std::string value = line.substr(offset + 1);

                    if (name == "peripheral_multiplier") {
                        m_peripheralResolutionFactor = std::stof(value);
                    } else if (name == "focus_multiplier") {
                        m_focusResolutionFactor = std::stof(value);
                    } else if (name == "no_eye_tracking") {
                        m_noEyeTracking = std::stoi(value);
                    } else if (name == "turbo_mode") {
                        m_useTurboMode = std::stoi(value);
                    } else {
                        Log("L%u: Unrecognized option\n", lineNumber);
                    }
                } else {
                    Log("L%u: Improperly formatted option\n", lineNumber);
                }
            } catch (...) {
                Log("L%u: Parsing error\n", lineNumber);
            }
        }

        bool m_bypassApiLayer{false};

        // Configuration.
        bool m_noEyeTracking{false};
        float m_peripheralResolutionFactor{1.f};
        float m_focusResolutionFactor{1.f};
        bool m_useTurboMode{false};

        // Foveated mode.
        std::mutex m_resourcesMutex;
        bool m_initialized{false};
        XrSpace m_viewSpace{XR_NULL_HANDLE};
        XrSpace m_renderGazeSpace{XR_NULL_HANDLE};

        // Turbo mode.
        std::chrono::time_point<std::chrono::steady_clock> m_lastFrameWaitTimestamp{};
        std::mutex m_frameLock;
        XrTime m_waitedFrameTime;
        std::mutex m_asyncWaitLock;
        std::future<void> m_asyncWaitPromise;
        XrTime m_lastPredictedDisplayTime{0};
        XrTime m_lastPredictedDisplayPeriod{0};
        bool m_asyncWaitPolled{false};
        bool m_asyncWaitCompleted{false};
    };

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

} // namespace

namespace openxr_api_layer {
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance() {
        g_instance.reset();
    }

} // namespace openxr_api_layer

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_PROCESS_DETACH:
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
