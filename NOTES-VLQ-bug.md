# Bug VLQ bloqué à 0 — résumé de la session

Branche : `claude/eager-knuth-4789he` (commit déjà poussé)

## Symptôme
- Les 4 RX reçoivent bien (vidéo propre), mais l'icône VLQ reste à 0 au lieu de 8.

## Chaîne VLQ dans le code
- Le VTX envoie une trame de config MSP (index `0xff`) ~8 fois/s, avec un compteur en octet 6.
- `src/driver/dm5680.c` : `DM5680_OSD_parse()` (UART1 seulement, état vidéo) appelle
  `recive_one_frame()` puis `lqStatistics()`.
- `src/core/msp_displayport.c` :
  - `parser_config()` -> `lqDetect(rx_buf[6])` compte `lq_rcv_cnt` / `lq_err_cnt`.
  - `lqStatistics()` calcule `link_quality` (0..8) une fois par seconde.
- `src/core/osd.c` : `osd_vlq_show()` affiche `VLQ<link_quality+1>.bmp` (VLQ9 = 8).
  Il affiche VLQ1 si aucun des deux RX n'est valide.

## Cause trouvée
`lqStatistics()` gardait `last_sec` en `uint16_t` alors que `time_s()` renvoie un `uint32_t`
basé sur l'horloge murale (`gettimeofday`). Quand l'heure du goggle est changée après le
démarrage (ou après ~18 h), `now != last_sec` est toujours vrai : les compteurs sont remis à
zéro à chaque paquet et VLQ ne dépasse jamais 0-1. Confirmé par l'utilisateur : il avait
changé l'heure.

## Correctif (commit poussé)
- `msp_displayport.c` : `last_sec` passe en `uint32_t`.
- `src/util/time.c` : `time_s()` utilise `CLOCK_MONOTONIC`.
- `lqDetect()` : le passage 255 -> 0 du compteur n'est plus compté comme perte
  (`(uint8_t)(last_lq + 1)`).
- Non testé sur matériel (pas de build complet ici).

## Second problème (pas lié à VLQ)
Après ~15 min, la vidéo devient inexploitable : perte de synchro vidéo, le cadenas
(latency lock) disparaît. VTX à 30 °C, donc pas de surchauffe.
- Le cadenas vient du registre FPGA `0x11` (`Get_VideoLatancy_status()`, < 4 = locked),
  lu dans `src/core/thread.c`. Aucun lien avec le correctif VLQ.
- Cause non identifiée.

## Modifs "démarrage à froid" des RX : ne tournent pas qu'au boot
Commits `f35a261` (DM5680 wait_ready, RXHEALTH) et `5e2213a` (DM6302 eFuse borné, relance init).
`DM6302_start()` (attente DM5680 jusqu'à 1,5 s, init radio, reset baseband) est exécuté par
chaque `HDZero_open()`, appelé aussi en vol :
- `scan_core_hdz_bw_tick()` (`src/core/scan_core.c`) : en mode bande « Both » (ou détection auto
  Box Pro / Goggle 2), si `rx_valid` tombe à 0, rouvre le récepteur sur l'autre bande toutes
  les ~4 s.
- `src/core/ht.c` : head tracker, réouverture après mouvement.
- Pages source / OSD / image settings / scan.
- Les lignes `RXHEALTH` (toutes les ~2 s la 1ʳᵉ minute, puis ~30 s) ne font que logger.

## Pistes pour la perte de synchro
1. Mode de bande HDZero : Wide, Narrow ou Both ? (Both = watchdog actif)
2. Chercher dans le log : `RXHEALTH open` / `open-suspect`, `DM5680 not ready`,
   `DM6302 init still suspect` juste avant la dégradation.
3. Relever RSSI / DLQ / `valid` / réponses par UART (`RXHEALTH run`) vers la 15ᵉ minute.
4. Tester sans changer l'heure, 15-20 min, pour voir si ça se reproduit.
5. Si ça n'arrive qu'avec ce firmware, comparer avec le firmware avant `f35a261` / `5e2213a`.
