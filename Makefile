CPP:=$(wildcard *.cpp)
OBJ:=$(patsubst %.cpp,obj/%.o,$(CPP))
LIBS:=-lfltk -lavcodec -lavformat -lavutil -lstdc++fs

obj/viewtune:	$(OBJ)
	g++ -o $@ $(OBJ) $(LIBS) -g

clean:
	rm -rf obj

obj/%.o:	%.cpp
	-mkdir -p obj
	g++ -c -o $@ $< -g -MMD -Wall -Werror -std=gnu++11 -Wno-unknown-pragmas
