include platform.mk

all: fibers.node

%.o: %.cc
	$(CXX) $(CPPFLAGS) -c -o $@ $^

fibers.node: fibers.o coroutine.o
	$(CXX) $(CPPFLAGS) $(CPP_NODEFLAGS) -o $@ $^ -lpthread

clean:
	-$(RM) fibers.node coroutine.o fibers.o
	-$(RM) -r *.dSYM
