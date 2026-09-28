/* Development-only SDK calls. Never edits reader databases. */
#include <inkview.h>

int main(int argc, char **argv) {
    InitInkview(TASK_NOHANDLER);
    if (argc == 3 && strcmp(argv[1], "describe") == 0) {
        char *end = NULL;
        long task = strtol(argv[2], &end, 10);
        if (!end || *end || task <= 0 || task > 2147483647L) return 2;
        taskinfo *info = GetTaskInfo((int)task);
        if (!info) return 1;
        printf("task=%d pid=%d flags=0x%x subtasks=%d app=%s\n", info->task,
               info->mainpid, info->flags, info->nsubtasks, info->appname);
        for (int i = 0; i < info->nsubtasks; ++i) {
            subtaskinfo *s = &info->subtasks[i];
            printf("subtask=%d book=%s\n", s->id, s->book ? s->book : "");
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "open-native") == 0) {
        const char prefix[] = "/mnt/ext1/books/BookOrbit/";
        if (strncmp(argv[2], prefix, sizeof(prefix) - 1) != 0) return 2;
        int result = OpenBook(argv[2], NULL, 0);
        printf("OpenBook=%d\n", result);
        return result <= 0;
    }
    if (argc == 2 && strcmp(argv[1], "close-reader") == 0) {
        int task = -1, subtask = -1;
        GetActiveTask(&task, &subtask);
        taskinfo *info = GetTaskInfo(task);
        if (!info || !info->appname || strcmp(info->appname, "/ebrmain/bin/eink-reader.app") != 0) {
            fprintf(stderr, "The foreground task is not the stock reader\n");
            return 2;
        }
        int result = CloseTask(task, ALLSUBTASKS, 0);
        printf("CloseTask task=%d subtask=%d force=0 result=%d\n", task, ALLSUBTASKS, result);
        return result < 0;
    }
    fprintf(stderr, "Usage: task_probe open-native PATH | close-reader | describe TASK\n");
    return 2;
}
