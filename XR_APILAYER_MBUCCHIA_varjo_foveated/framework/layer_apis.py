# The list of OpenXR functions our layer will override.
override_functions = [
    "xrEnumerateViewConfigurationViews",
    "xrCreateSwapchain",
    "xrDestroySwapchain",
    "xrBeginSession",
    "xrDestroySession",
    "xrLocateViews",
    "xrWaitFrame",
    "xrBeginFrame",
    "xrEndFrame",
    "xrAcquireSwapchainImage",
    "xrWaitSwapchainImage",
    "xrReleaseSwapchainImage"
]

# The list of OpenXR functions our layer will use from the runtime.
# Might repeat entries from override_functions above.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystem",
    "xrGetSystemProperties",
    "xrGetViewConfigurationProperties",
    "xrCreateReferenceSpace",
    "xrLocateSpace"
]

# The list of OpenXR extensions our layer will either override or use.
extensions = []
