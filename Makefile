OBJ=\
deps/rax/rax.o\
lib/random.o\
lib/fact.o\
lib/set.o\
lib/intern.o\
lib/arena.o\
lib/rw.o\
lib/binding.o\
lib/skiplist.o\
lib/facts.o\
lib/spec.o



TEST_UTILS=\
t/00util/test/check_fact\
t/00util/test/check_facts\
t/00util/test/check_intern\
t/00util/test/check_set\
t/00util/test/check_skiplist\
t/00util/test/check_spec


BENCH_UTILS=\
t/00util/bench/benchmark_facts_add\
t/00util/bench/benchmark_facts_with\
t/00util/bench/benchmark_set_add\
t/00util/bench/benchmark_set_add_overflow\
t/00util/bench/benchmark_set_get\
t/00util/bench/benchmark_set_remove

CPPFLAGS = -Iinclude -Ideps/rax -D_DEFAULT_SOURCE
CFLAGS = -DNDEBUG -Os -g -W -Wall -Werror -std=c11 -pedantic -fPIC
LDLIBS = -lm

all: libfacts_db.a

t/00util/bench/benchmark_facts_add: t/00util/bench/benchmark_facts_add.o $(OBJ)

t/00util/bench/benchmark_facts_with: t/00util/bench/benchmark_facts_with.o $(OBJ)

t/00util/bench/benchmark_set_add: t/00util/bench/benchmark_set_add.o $(OBJ)

t/00util/bench/benchmark_set_add_overflow: t/00util/bench/benchmark_set_add_overflow.o $(OBJ)

t/00util/bench/benchmark_set_get: t/00util/bench/benchmark_set_get.o $(OBJ)

t/00util/bench/benchmark_set_remove: t/00util/bench/benchmark_set_remove.o $(OBJ)

t/00util/test/check_fact: t/00util/test/check_fact.o $(OBJ)

t/00util/test/check_facts: t/00util/test/check_facts.o $(OBJ)

t/00util/test/check_intern: t/00util/test/check_intern.o $(OBJ)

t/00util/test/check_set: t/00util/test/check_set.o $(OBJ)

t/00util/test/check_skiplist: t/00util/test/check_skiplist.o $(OBJ)

t/00util/test/check_spec: t/00util/test/check_spec.o $(OBJ)

check: CFLAGS=-O0 -g -W -Wall -Werror -std=c11 -pedantic -Wno-unused
check: $(TEST_UTILS) $(BENCH_UTILS);
	prove -I. -v t/*.t

libfacts_db.a: $(OBJ)
	$(AR) rcs $@ $(OBJ) 

clean:
	$(RM) *.o *.a *.gperf *.prof t/00util/bench/*.o t/00util/test/*.o $(TEST_UTILS) $(BENCH_UTILS) $(OBJ)

indent:
	find -name '*.[h,c]' | xargs clang-format -i

scan:
	scan-build $(MAKE) clean all

