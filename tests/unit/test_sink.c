/* sink 单元测试：用 sink_null 验证 vtable 分发、参数校验、生命周期 */
#include <stdio.h>
#include "camera/sink.h"

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                          \
    tests_run++;                                                  \
    if (!(cond)) {                                                \
        tests_failed++;                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                             \
} while (0)

int main(void)
{
    frame_sink *s = sink_null_create();
    CHECK(s != NULL);
    CHECK(sink_null_frame_count(s) == 0);

    unsigned char rgb[12] = {0};   /* 2x2 RGB24，内容无所谓 */
    CHECK(sink_render(s, rgb, sizeof(rgb)) == 0);
    CHECK(sink_render(s, rgb, sizeof(rgb)) == 0);
    CHECK(sink_render(s, rgb, sizeof(rgb)) == 0);
    CHECK(sink_null_frame_count(s) == 3);   /* 3 帧都流过了 */

    /* 参数校验 */
    CHECK(sink_render(NULL, rgb, sizeof(rgb)) == -1);   /* sink 为空 */
    CHECK(sink_render(s, NULL, sizeof(rgb)) == -1);     /* 数据为空 */
    CHECK(sink_render(s, rgb, 0) == -1);                /* 大小为 0 */

    sink_destroy(NULL);   /* NULL 销毁应安全返回 */
    sink_destroy(s);

    printf("== %d 个断言, %d 个失败 ==\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
