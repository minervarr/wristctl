// Verifies the C core is consumable from C++ exactly as shipped: the headers
// carry extern "C" guards and the structs are plain aggregates.
#include <cstdio>
#include <cstring>

#include "wristctl/generated.h"
#include "wristctl/models.h"

namespace {

int failures = 0;

void check(const char *what, bool ok)
{
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main()
{
    // 2026-10-27 18:02:01, weekday 2, from the recorded capture.
    const unsigned char frame[WC_LEN_CURRENT_TIME] = {
        0x09, 0xea, 0x07, 0x0a, 0x1b, 0x12, 0x02, 0x01, 0x02, 0x00, 0x01,
    };

    wc_current_time value{};
    const int rc = wc_current_time_decode(frame, sizeof frame, &value);
    check("decode rc", rc == WC_OK);
    check("year", value.year == 2026);
    check("month", value.month == 10);
    check("day", value.day == 27);
    check("hour", value.hour == 18);
    check("minute", value.minute == 2);
    check("second", value.second == 1);
    check("weekday", value.weekday == 2);

    unsigned char out[WC_LEN_CURRENT_TIME] = {};
    std::size_t n = 0;
    check("encode rc", wc_current_time_encode(&value, out, sizeof out, &n) == WC_OK);
    check("round trip", n == sizeof frame && std::memcmp(out, frame, n) == 0);

    const wc_model *model = wc_model_lookup("CASIO GBX-100");
    check("model lookup", model != nullptr);
    if (model != nullptr) {
        check("model name", std::strcmp(model->name, "GBX_100") == 0);
        check("tide flag", wc_model_flag(model, "isEnableTideData") == 1);
    }

    const wc_class_info *info = wc_class_by_id(0x09);
    check("class lookup", info != nullptr && info->length == WC_LEN_CURRENT_TIME);

    if (failures == 0) {
        std::printf("ok c++ consumer: %zu classes, %zu models\n",
                    wc_class_count(), wc_model_count);
    }
    return failures == 0 ? 0 : 1;
}
