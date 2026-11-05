#ifndef SWEET_WLAN_UTIL_H
#define SWEET_WLAN_UTIL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct swu_assoc {
	uint8_t bssid[6], sa[6];
	uint16_t cap;
	uint8_t ssid[32];
	uint8_t ssid_len;
	const uint8_t *ap_ies;
	size_t ap_ies_len;
	const uint8_t *rsn_ie;
	size_t rsn_ie_len;
};

struct swu_assoc_resp {
	uint16_t cap, status, aid;
	uint8_t rates[32], nrates;
	bool wmm;
	bool ht;
	uint16_t ht_caps;
	uint8_t ht_ampdu;
};

struct swu_wpa_ops {
	void (*tx)(void *arg, const uint8_t *eapol, size_t len);
	int (*key)(void *arg, int pairwise, unsigned idx, const uint8_t *key, size_t len);
	void (*done)(void *arg, int status);
	void (*random)(uint8_t *buf, size_t len);
};

struct swu_wpa {
	const struct swu_wpa_ops *ops;
	void *arg;
	uint8_t pmk[32], aa[6], spa[6], anonce[32], snonce[32], ptk[48], replay[8], gtk[32], rsn[64];
	size_t rsn_len;
	unsigned gtk_len, gtk_idx, rx, rekeys;
	int state;                      /* 0 idle, 1 message 2 sent, 2 message 4 sent */
	bool have_ptk;
};

void swu_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[20]);
const uint8_t *swu_ie_find(const uint8_t *ies, size_t len, uint8_t id);
size_t swu_auth_req(uint8_t *f, size_t size, const uint8_t bssid[6], const uint8_t sa[6]);
size_t swu_deauth(uint8_t *f, size_t size, const uint8_t bssid[6], const uint8_t sa[6], uint16_t reason);
size_t swu_build_rsn(uint8_t *out, uint8_t group_type);
extern int swu_no_ht;
size_t swu_assoc_req(uint8_t *f, size_t size, const struct swu_assoc *a);
int swu_auth_resp(const uint8_t *f, size_t len, const uint8_t bssid[6], const uint8_t me[6], uint16_t *status);
int swu_assoc_resp(const uint8_t *f, size_t len, const uint8_t bssid[6], const uint8_t me[6], struct swu_assoc_resp *r);
int swu_kicked(const uint8_t *f, size_t len, const uint8_t bssid[6], const uint8_t me[6], uint16_t *reason);
size_t swu_eth_to_nwifi(const uint8_t *eth, size_t len, const uint8_t bssid[6], uint8_t *out, size_t size);
size_t swu_nwifi_to_eth(const uint8_t *f, size_t len, uint8_t *eth, size_t size);
void swu_wpa_start(struct swu_wpa *w, const struct swu_wpa_ops *ops, void *arg, const uint8_t pmk[32], const uint8_t aa[6],
    const uint8_t spa[6], const uint8_t *sta_rsn, size_t sta_rsn_len);
void swu_wpa_stop(struct swu_wpa *w);
int swu_wpa_rx(struct swu_wpa *w, const uint8_t *f, size_t len);
#endif
