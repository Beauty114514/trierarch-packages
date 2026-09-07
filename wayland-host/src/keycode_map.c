#include "keycode_map.h"

/* Android KeyEvent keycodes mapped to Linux evdev codes. Scan codes are not a
 * portable Android ABI, so the host uses this stable mapping first. */
uint32_t trierarch_android_keycode_to_evdev(int keycode) {
    switch (keycode) {
    case 7: return 11; case 8: return 2; case 9: return 3; case 10: return 4;
    case 11: return 5; case 12: return 6; case 13: return 7; case 14: return 8;
    case 15: return 9; case 16: return 10;
    case 19: return 103; case 20: return 108; case 21: return 105; case 22: return 106;
    case 28: return 28;
    case 29: return 30; case 30: return 48; case 31: return 46; case 32: return 32;
    case 33: return 18; case 34: return 33; case 35: return 34; case 36: return 35;
    case 37: return 23; case 38: return 36; case 39: return 37; case 40: return 38;
    case 41: return 50; case 42: return 49; case 43: return 24; case 44: return 25;
    case 45: return 16; case 46: return 19; case 47: return 31; case 48: return 20;
    case 49: return 22; case 50: return 47; case 51: return 17; case 52: return 45;
    case 53: return 21; case 54: return 44; case 55: return 51; case 56: return 52;
    case 57: return 56; case 58: return 100; case 59: return 42; case 60: return 54;
    case 61: return 15; case 62: return 57; case 66: return 28; case 67: return 14;
    case 68: return 41; case 69: return 12; case 70: return 13; case 71: return 26;
    case 72: return 27; case 73: return 43; case 74: return 39; case 75: return 40;
    case 76: return 53; case 92: return 104; case 93: return 109;
    case 111: return 1; case 112: return 111; case 113: return 29; case 114: return 97;
    case 115: return 58; case 116: return 70; case 117: return 125; case 118: return 126;
    case 122: return 102; case 123: return 107; case 124: return 110;
    case 131: return 59; case 132: return 60; case 133: return 61; case 134: return 62;
    case 135: return 63; case 136: return 64; case 137: return 65; case 138: return 66;
    case 139: return 67; case 140: return 68; case 141: return 87; case 142: return 88;
    case 143: return 69;
    default: return 0;
    }
}
