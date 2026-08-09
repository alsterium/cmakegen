#if defined(PLATFORM_MACOS)
int macos_value();
int platform_value() { return macos_value(); }
#elif defined(PLATFORM_LINUX)
int linux_value();
int platform_value() { return linux_value(); }
#elif defined(PLATFORM_WINDOWS)
int windows_value();
int platform_value() { return windows_value(); }
#else
#error "a platform override must define PLATFORM_*"
#endif
