/* Loader probe (#240): built outside the kernel and passed as a Multiboot
 * module; prints its arguments and environment. */

#include <stdio.h>
#include <stdlib.h>

extern char** environ;

int main(int argc, char** argv) {
    printf("args: argc %d", argc);
    for (int i = 0; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\nargs: env");
    for (char** e = environ; *e; e++) printf(" [%s]", *e);
    const char* foo = getenv("FOO");
    printf("\nargs: FOO=%s\n", foo ? foo : "(unset)");
    return argc == 3 ? 0 : 1;
}
