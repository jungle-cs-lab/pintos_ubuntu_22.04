#include <random.h>
#include "tests/lib.h"
#include "tests/main.h"

int main(int argc UNUSED, char* argv[])
{
    test_name = argv[0];

    msg("begin");
    random_init(0);
    test_main();
    msg("end");
    return 0;
}
// pintos --fs-disk=10 -p tests/userprog/args-none:args-none --swap-disk=4 -- -q -f run 'args-none'