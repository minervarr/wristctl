/* Runs the generated golden vectors against the generated C codecs. */
#include "generated_vectors.h"

int main(void)
{
    int failures = 0;
    for (size_t i = 0; i < vec_entry_count; i++) {
        if (vec_entries[i].fn() != 0) {
            failures++;
        } else {
            printf("ok %s\n", vec_entries[i].name);
        }
    }
    printf("%zu checks, %d failure(s)\n", vec_entry_count, failures);
    return failures == 0 ? 0 : 1;
}
