CPP:=$(wildcard *.cpp)
OBJ:=$(patsubst %.cpp,obj/%.o,$(CPP))
LIBS:=-lfltk -lavcodec -lavformat -lavutil -lstdc++fs -lpthread
OBJ_gobble:=$(filter-out obj/viewtune.o,$(OBJ))
OBJ_viewtune:=$(filter-out obj/gobble.o,$(OBJ))

all:	obj/gobble obj/viewtune

obj/gobble:	$(OBJ_gobble)
	g++ -o $@ $(OBJ_gobble) $(LIBS) -g

obj/viewtune:	$(OBJ_viewtune)
	g++ -o $@ $(OBJ_viewtune) $(LIBS) -g

clean:
	rm -rf obj

obj/%.o:	%.cpp
	-mkdir -p obj
	g++ -c -o $@ $< -g -MMD -Wall -Werror -std=gnu++11 -Wno-unknown-pragmas
