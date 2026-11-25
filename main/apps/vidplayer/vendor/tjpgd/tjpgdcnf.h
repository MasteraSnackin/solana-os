/* TJpgDec config for Solana OS vidplayer — fixed, no sdkconfig. */
#pragma once

#define JD_SZBUF        512
#define JD_FORMAT       1       /* RGB565 */
#define JD_USE_SCALE    1
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1
#define JD_DEFAULT_HUFFMAN 0

/* ESP32-C5 ROM exports jd_prepare/jd_decomp absolutely; rename ours. */
#define jd_prepare vidplayer_jd_prepare
#define jd_decomp  vidplayer_jd_decomp
