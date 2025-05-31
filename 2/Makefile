GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all:
	gcc $(GCC_FLAGS) solution.c parser.c

h:
	gcc $(GCC_FLAGS) solution.c parser.c ../utils/heap_help/heap_help.c -ldl -rdynamic
# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c -o mybash
