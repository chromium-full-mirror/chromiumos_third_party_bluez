static inline void BluetoothAdapterStateChanged(const char* boot_id,
                                                int64_t system_time,
                                                int state) {}

static inline void BluetoothPairingStateChanged(const char* boot_id,
                                                int64_t system_time,
                                                const char* device_id,
                                                int device_type,
                                                int state) {}

static inline void BluetoothAclConnectionStateChanged(const char* boot_id,
                                                      int64_t system_time,
                                                      const char* device_id,
                                                      int device_type,
                                                      int connection_direction,
                                                      int connection_initiator,
                                                      int state_change_type,
                                                      int state) {}

void BluetoothProfileConnectionStateChanged(const char* boot_id,
                                            int64_t system_time,
                                            const char* device_id,
                                            int state_change_type,
                                            int profile,
                                            int state) {}

void BluetoothDeviceInfoReport(const char* boot_id,
                               int64_t system_time,
                               const char* device_id,
                               int device_type,
                               int device_class,
                               int device_category,
                               int vendor_id,
                               int vendor_id_source,
                               int product_id,
                               int product_version) {}

void BluetoothDeviceInfo(int device_type,
                         int device_class,
                         int device_category,
                         int vendor_id,
                         int vendor_id_source,
                         int product_id,
                         int product_version) {}

void BluetoothChipsetInfo(int vendor_id,
                          int product_id,
                          int transport,
                          const char* chipset_string) {}
