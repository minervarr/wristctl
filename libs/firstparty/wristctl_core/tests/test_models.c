/* Model registry tests: longest-alias matching and capability flags. */
#include <stdio.h>
#include <string.h>

#include "wristctl/models.h"

static int failures;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL models: %s\n", what); failures++; } \
} while (0)

int main(void)
{
    const wc_model *gbx = wc_model_lookup("CASIO GBX-100");
    CHECK(gbx != NULL, "GBX-100 lookup");
    CHECK(gbx != NULL && strcmp(gbx->name, "GBX_100") == 0, "GBX-100 name");
    CHECK(gbx != NULL && gbx->generation == 22, "GBX-100 generation");

    const wc_model *gst1000 = wc_model_lookup("CASIO GST-B1000");
    const wc_model *gst100 = wc_model_lookup("CASIO GST-B100");
    CHECK(gst1000 != NULL && strcmp(gst1000->name, "GST_B1000") == 0, "longest alias GST-B1000");
    CHECK(gst100 != NULL && strcmp(gst100->name, "GST_B100") == 0, "longest alias GST-B100");

    CHECK(wc_model_lookup("ACME WATCH 3000") == NULL, "unknown name");
    CHECK(wc_model_lookup(NULL) == NULL, "NULL name");

    CHECK(wc_model_flag(gbx, "isEnableTideData") == 1, "GBX-100 tide");
    CHECK(wc_model_flag(wc_model_lookup("CASIO GBD-H1000"), "isEnableTideData") == 0, "H1000 tide");
    CHECK(wc_model_flag(gbx, "isNeedCreateBond") == 0, "GBX-100 no bond");
    CHECK(wc_model_flag(wc_model_lookup("CASIO GBD-H2000"), "isNeedCreateBond") == 1, "H2000 bond");
    CHECK(wc_model_flag(gbx, "notAFlag") == -1, "unknown flag");
    CHECK(wc_model_flag(NULL, "isEnableTideData") == -1, "NULL model");

    CHECK(wc_model_count == 137, "model count");
    CHECK(wc_flag_count == 107, "flag count");

    if (failures == 0) {
        printf("ok models: %zu models, %zu flags\n", wc_model_count, wc_flag_count);
    }
    return failures == 0 ? 0 : 1;
}
