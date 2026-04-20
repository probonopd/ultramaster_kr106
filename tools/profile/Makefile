CXX ?= clang++
CXXFLAGS = -std=c++17 -O3 -DNDEBUG -ffp-contract=fast -fno-math-errno -Wall -Wextra
DEPS = $(wildcard ../../Source/DSP/*.h) ../../Source/KR106_Presets_JUCE.h

all: dsp_profile osc_ab_test osc_render

dsp_profile: dsp_profile.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ dsp_profile.cpp

osc_ab_test: osc_ab_test.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ osc_ab_test.cpp

osc_render: osc_render.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ osc_render.cpp

clean:
	rm -f dsp_profile osc_ab_test osc_render

.PHONY: clean
