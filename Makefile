OBJ=\
deps/rax/rax.o\
lib/random.o\
lib/fact.o\
lib/set.o\
lib/intern.o\
lib/arena.o\
lib/io.o\
lib/binding.o\
lib/hexastore.o\
lib/lftj.o\
lib/transaction.o\
lib/sparql.o\
lib/facts.o\
lib/spec.o\
lib/rule.o\
lib/eval.o\
lib/justification.o\
lib/magic.o\
lib/urcu.o\
lib/linda.o




TEST_UTILS=\
t/00util/test/check_fact\
t/00util/test/check_facts\
t/00util/test/check_intern\
t/00util/test/check_set\
t/00util/test/check_spec\
t/00util/test/check_triejoin\
t/00util/test/check_movie_integration\
t/00util/test/check_sparql\
t/00util/test/check_rule\
t/00util/test/check_eval\
t/00util/test/check_magic\
t/00util/test/check_linda



BENCH_UTILS=\
t/00util/bench/benchmark_facts_add\
t/00util/bench/benchmark_facts_with\
t/00util/bench/benchmark_set_add\
t/00util/bench/benchmark_set_add_overflow\
t/00util/bench/benchmark_set_get\
t/00util/bench/benchmark_set_remove\
t/00util/bench/benchmark_reactive_retraction

CPPFLAGS = -Iinclude -Ideps/rax -D_DEFAULT_SOURCE
CFLAGS = -DNDEBUG -Os -g -W -Wall -Werror -std=c11 -pedantic -fPIC -pthread
CPPFLAGS += -MMD -MP
LDLIBS = -lm -pthread

DEPS=$(OBJ:.o=.d) $(TEST_UTILS:=.d) $(BENCH_UTILS:=.d) sparql_repl.d deps/linenoise/linenoise.d
BUILD_CONFIG=.build-config

-include $(DEPS)

.PHONY: FORCE
FORCE:

$(BUILD_CONFIG): FORCE
	@config='CC=$(CC) CPPFLAGS=$(CPPFLAGS) CFLAGS=$(CFLAGS) LDFLAGS=$(LDFLAGS) LDLIBS=$(LDLIBS)'; \
	printf '%s\n' "$$config" | cmp -s - $@ || printf '%s\n' "$$config" > $@

$(OBJ) $(TEST_UTILS:=.o) $(BENCH_UTILS:=.o) sparql_repl.o deps/linenoise/linenoise.o: $(BUILD_CONFIG)

all: libdatalog.a sparql_repl

t/00util/bench/benchmark_facts_add: t/00util/bench/benchmark_facts_add.o $(OBJ)

t/00util/bench/benchmark_facts_with: t/00util/bench/benchmark_facts_with.o $(OBJ)

t/00util/bench/benchmark_set_add: t/00util/bench/benchmark_set_add.o $(OBJ)

t/00util/bench/benchmark_set_add_overflow: t/00util/bench/benchmark_set_add_overflow.o $(OBJ)

t/00util/bench/benchmark_set_get: t/00util/bench/benchmark_set_get.o $(OBJ)

t/00util/bench/benchmark_set_remove: t/00util/bench/benchmark_set_remove.o $(OBJ)

t/00util/bench/benchmark_reactive_retraction: t/00util/bench/benchmark_reactive_retraction.o $(OBJ)

t/00util/test/check_fact: t/00util/test/check_fact.o $(OBJ)

t/00util/test/check_facts: t/00util/test/check_facts.o $(OBJ)

t/00util/test/check_intern: t/00util/test/check_intern.o $(OBJ)

t/00util/test/check_set: t/00util/test/check_set.o $(OBJ)

t/00util/test/check_spec: t/00util/test/check_spec.o $(OBJ)

t/00util/test/check_triejoin: t/00util/test/check_triejoin.o $(OBJ)

t/00util/test/check_movie_integration: t/00util/test/check_movie_integration.o $(OBJ)

t/00util/test/check_sparql: t/00util/test/check_sparql.o $(OBJ)

t/00util/test/check_rule: t/00util/test/check_rule.o $(OBJ)

t/00util/test/check_eval: t/00util/test/check_eval.o $(OBJ)

t/00util/test/check_magic: t/00util/test/check_magic.o $(OBJ)

t/00util/test/check_linda: t/00util/test/check_linda.o $(OBJ)


check: CFLAGS=-O2 -g -W -Wall -Werror -std=c11 -pedantic -Wno-unused -pthread
check: $(TEST_UTILS) $(BENCH_UTILS);
	prove -I. -v t/*.t

libdatalog.a: $(OBJ)
	$(AR) rcs $@ $(OBJ) 

sparql_repl: sparql_repl.o deps/linenoise/linenoise.o $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)


clean:
	$(RM) *.o *.a *.gperf *.prof t/00util/bench/*.o t/00util/test/*.o deps/linenoise/*.o sparql_repl $(TEST_UTILS) $(BENCH_UTILS) $(OBJ) $(DEPS) $(BUILD_CONFIG)


indent:
	find -name '*.[h,c]' | xargs clang-format -i

scan:
	scan-build $(MAKE) clean all

check-asan:
	ASAN_OPTIONS=detect_leaks=0 $(MAKE) -B check CFLAGS='-O1 -g -W -Wall -Werror -std=c11 -pedantic -Wno-unused -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

check-tsan:
	$(MAKE) -B t/00util/test/check_linda CFLAGS='-O1 -g -W -Wall -Werror -std=c11 -pedantic -Wno-unused -pthread -fsanitize=thread -fno-omit-frame-pointer' LDFLAGS='-fsanitize=thread'
	./t/00util/test/check_linda

.PHONY: all check check-asan check-tsan clean indent scan
