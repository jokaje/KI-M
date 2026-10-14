/* Device-local provisioning. Copy to config_local.h, never commit deployed values.
 * HTTPS server URL must name the certificate SAN; PEM is the issuing CA.
 * No implicit insecure HTTP or TLS skip-verification mode exists. */
#define KIM_WIFI_SSID ""
#define KIM_WIFI_PASSWORD ""
#define KIM_SERVER_URL ""
#define KIM_DEVICE_TOKEN ""
#define KIM_SERVER_CA_PEM ""
