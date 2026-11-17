# TODO — ssv6xxx-ng

Feature da implementare in seguito, in ordine di priorità.

## 1. WPA3 / PMF via crypto software
**Stato: percorso individuato, da implementare dopo il bring-up su HW.**

Il chip esegue in hardware solo WEP40/104, TKIP e CCMP-128 (chiavi caricate
nella SRAM del chip via `set_key` → `SSV_CIPHER_*`). Per WPA3 servono cipher
che l'hardware non conosce, ma mac80211 li implementa in software:

- `IEEE80211_HW_MFP_CAPABLE` — abilita 802.11w (PMF); BIP-CMAC-128/256 è
  gestito interamente da mac80211 (`net/mac80211/aes_cmac.c`,
  `net/mac80211/aes_gmac.c`), il driver non riceve le chiavi BIP.
- GCMP-128 / GCMP-256 / CCMP-256: dichiarare i cipher in
  `hw->wiphy->cipher_suites` e rifiutare le chiavi in `set_key` con
  `-EOPNOTSUPP`; mac80211 fa il fallback a crittografia software
  (`net/mac80211/gcmp.c`, `net/mac80211/ccmp.c`).
- SAE (handshake WPA3-Personal): interamente in wpa_supplicant, nessun
  lavoro nel driver.
- Rischi da verificare su HW: performance (crypto su CPU host), e che il
  firmware 12978 tolleri frame dati già cifrati dall'host (il path TX
  inoltra frame cifrati senza toccarli, ma il firmware potrebbe fare
  controlli sul campo `security` del descrittore).

## 2. Modalità AP e P2P
- `ieee80211_ops.start_ap` / `.stop_ap`, beaconing via firmware.
- `NL80211_IFTYPE_AP` e `NL80211_IFTYPE_P2P_*` in `interface_modes`.
- WSID aggiuntivi (il chip ne supporta 8, v1 ne usa 1 per la STA).

## 3. AMPDU / aggregazione TX
- `IEEE80211_TX_CTL_AMPDU` nel path TX, block-ack via eventi
  `SOC_EVT_RC_AMPDU_REPORT` / `SOC_EVT_NO_BA`.
- Il descrittore TX ha già i campi `aggregation` e `rc_params`.

## 4. RX quality: RSSI reale
- v1 riporta un segnale fisso (-50 dBm); estrarre RPCI/SNR reali da
  `ssv6051_rxphy_info` (campi `rpci`/`snr`) e mappare su
  `rx_status->signal`.

## 5. Rate control automatico
- Il firmware ha un rate controller (`SOC_EVT_RC_MPDU_REPORT`); v1 usa
  rate fissi. Collegare `ieee80211_start_tx_ba_session` e il reporting
  MPDU per abilitare il rate control automatico.

## 6. Power management avanzato
- `ssv_pm.c` del driver originale: deep sleep, wake-on-wireless.
- `REG_PMU_WAKEUP` già usato dal transport SDIO.

## 7. Raffinamenti efuse/calibrazione (opzionali)
- Override calibrazione per-board (file config).
- Dump efuse via debugfs (già presente `regdump`).
- Aggiustamenti custom potenza TX (`tx_power_index_1/2`).
- WAPI/SMS4: da rivalutare (richiede verifica firmware).

## 8. Divergenze funzionali residue rispetto al legacy
Differenze note ng ⇄ legacy emerse dal confronto durante il bring-up
(2024-11); nessuna crashante, ma ognuna può degradare qualità o
resilienza.

### 8.0 `.conf_tx` (parametri EDCA) — implementato durante il bring-up
- Il legacy implementa `.conf_tx` (`ssv6200_conf_tx`): scrive AIFS/
  ECWmin/ECWmax/TXOP in `ADR_TXQ0_MTX_Q_AIFSN + 0x100 * hw_txqid` e il
  bit QOS_EN in `ADR_GLBLE_SET`. Il ng non lo implementava: la scheda
  restava con timing di accesso al canale non configurato — candidato
  principale per i frame presi dalla coda ma mai trasmessi (TX ID mai
  rilasciati).
- Ora implementato in `main.c` (`ssv6051_conf_tx`), stessa formula del
  legacy (aifs-1, ilog2(cw+1), txop).

### 8.1 Watchdog heartbeat bidirezionale
- Il legacy ha un timer host (10s, `ssv6200_watchdog_timeout`): il
  firmware manda periodicamente `SOC_EVT_WATCHDOG_TRIGGER`, il timer
  verifica; se nessun ping nell'intervallo → firmware muto →
  `ssv6xxx_watchdog_restart_hw`.
- Il ng gestisce gli eventi quando arrivano (`rx.c` →
  `ssv6051_restart_hw`) ma NON ha il timer: un firmware morto senza
  emettere l'evento non viene mai rilevato → scan/auth appesi all'infinito.
- Da fare: timer 10s armato in `.start` (legacy: `watchdog_flag = 2`,
  `mod_timer(5*HZ)`), clear del flag su evento, restart su scadenza.

### 8.2 Soglie CCA 11b durante lo scan
- Il legacy in `sw_scan_start` salva `ADR_RX_11B_CCA_CONTROL` e
  `ADR_RX_11B_CCA_1` e scrive `RX_11B_CCA_IN_SCAN` (riduce i falsi CCA
  da traffico 11b durante la scansione); `sw_scan_complete` ripristina.
- Il ng: stub vuoti. Impatto: sensibilità di scan in presenza di
  traffico 11b, non stabilità.

### 8.3 Gestione PS (IEEE80211_CONF_CHANGE_PS)
- Il legacy la intercetta in `.config` (stamp diagnostico + macchina di
  stato PS in `ssv_pm.c` con `HOST_CMD_PS` runtime).
- Il ng la ignora; invia `HOST_CMD_PS` solo allo scarico
  (`ssv6051_hci_fw_sleep`). Impatto: nessun power save host-side
  (consumo), nessun wedge noto.

### 8.4 Rate TX: drate_idx/crate_idx
**Stato: implementato (drate_idx da mac80211, crate_idx al rate base).**
- Il legacy li ricava dal rate control (`ssv6xxx_rc_hw_rate_idx`); il
  ng ora usa `info->control.rates[0].count` (l'indice nella tabella
  rate del driver, stessa numerazione dell'hw), con fallback al rate
  base per gli indici MCS (il ng non fa HT TX). `crate_idx` resta al
  rate base come nel percorso CTS-protect del legacy.
- Residuo (voce 5): rate control automatico con feedback MPDU — ora il
  rate è quello scelto da mac80211 all'incodamento e non si adatta
  (il tx_status finto non dà feedback al RC).

### 8.5 duration_id / frame duration
- Il legacy calcola `hdr->duration_id` via `ssv6xxx_set_frame_duration`
  (NAV per frame non-aggregato); il ng lascia quello che mette mac80211.
  Impatto: possibile degrado in ambienti affollati (NAV mancante).

### 8.6 Flow control TX: interrupt di risorsa
- Il legacy ri-arma l'invio per coda via interrupt TX-done della scheda
  (bit per TXQ in `SSV6XXX_INT_TX`); il ng usa un retry differito (20ms,
  `tx_retry`) dopo uno stall di risorse. Impatto: latenza in coda in
  condizioni di carico elevato; da sostituire con l'interrupt quando si
  implementa la voce 3 (AMPDU).

### 8.7 TXQ flush allo stop
- Il legacy fa `HCI_TXQ_FLUSH` in `ssv6200_stop` restituendo i frame
  rimasti a mac80211; il ng li scarta (`tx_dropped`) se arriva un TX
  work dopo lo stop. Impatto: nessuno per la stabilità (gli skb sono
  comunque liberati); da allineare se si vuole il riuso dei frame.

## 9. Cifratura HW: limiti noti
Implementata per interfaccia STA singola (AP = WSID HW 0, `key.c`):
`sta_add`/`sta_remove`, chiavi WEP/TKIP/CCMP in SRAM, `M_ENG_ENCRYPT` in TX,
`M_ENG_ENCRYPT_SEC` in RX. Non ancora fatto:
- dopo `ssv6051_restart_hw` WSID, chiavi e tipo cifrario nel chip vanno
  riprogrammati dallo shadow `sdev->sec_shadow`;
- niente WSID > 0 (TDLS, AP mode): il legacy usa `FW_WSID_WATCH_LIST`
  con tabelle chiavi per WSID a offset 0x10000;
- verifica su HW dei comandi firmware `WSID_OP` (SET_TYPE SW/HW).

