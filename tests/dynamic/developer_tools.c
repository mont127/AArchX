#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
    FILE *p = popen("xcrun --show-sdk-path 2>&1", "r");
    char line[1024] = "";
    if (!p || !fgets(line, sizeof line, p)) {
        puts("xcrun printed nothing");
        return 1;
    }
    int st = pclose(p);
    if (line[0] != '/' || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("xcrun failed: %s", line);
        return 1;
    }
    puts("OK");
    return 0;
}
