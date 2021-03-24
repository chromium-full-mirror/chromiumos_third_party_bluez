typedef void* CMetricsLibrary;

static inline CMetricsLibrary CMetricsLibraryNew() {
	return NULL;
}

static inline void CMetricsLibraryDelete(CMetricsLibrary l) {
	return;
}

static inline int CMetricsLibraryAreMetricsEnabled() {
	return 0;
}

static inline int CMetricsLibrarySendToUMA(CMetricsLibrary handle,
                             const char* name,
                             int sample,
                             int min,
                             int max,
                             int nbuckets) {
	return 0;
}

static inline int CMetricsLibrarySendEnumToUMA(CMetricsLibrary handle,
                                 const char* name,
                                 int sample,
                                 int max) {
	return 0;
}
