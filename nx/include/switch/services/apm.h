/// These are used internally by applet.

Result apmInitialize(void);
void apmExit(void);

Result apmSetPerformanceConfiguration(u32 PerformanceMode, u32 PerformanceConfiguration);
Result apmGetPerformanceConfiguration(u32 PerformanceMode, u32 *PerformanceConfiguration);
