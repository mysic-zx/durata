/* 测试入口：MP3 状态机 + 容器格式 + 公共 API */
#include "du_test.h"

int du_check_fail = 0;
int du_total_fail = 0;

void du_test_mp3(void);
void du_test_containers(void);

int main(void) {
    du_test_mp3();
    du_test_containers();
    printf("\n%d check(s) failed\n", du_total_fail);
    return du_total_fail == 0 ? 0 : 1;
}
