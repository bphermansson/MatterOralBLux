#include <stdint.h>
#include "advertisement_message.h"

const char *advertisement_message(uint8_t state)
{
    switch (state) {
    case 2:
        return "Brush idle";
    case 3:
        return "Working";
    case 4:
        return "Charging";
    case 8:
        return "Brush menu";
    case 9:
    case 10:
        return "Session done";
    default:
        return "Brush detected";
    }
}