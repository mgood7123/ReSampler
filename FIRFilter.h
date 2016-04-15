#ifndef FIRFFILTER_H_
#define FIRFFILTER_H_

#include <typeinfo>

#define FILTERSIZE_HUGE 32767
#define FILTERSIZE_MEDIUM 511

#define USE_SSE2 1 // Use SSE2-specific intrinsics in the build

#if (defined(_M_X64) || defined(USE_SSE2)) // All x64 CPUs have SSE2 instructions, but some older 32-bit CPUs do not. 
	#define USE_SIMD 1 // Vectorise main loop in FIRFilter::get() by using SSE2 SIMD instrinsics 
	// 2016/04/01: Needs specializations (4xfloat SIMD code won't work for double precision)
#endif

template <typename FloatType, unsigned int size>
class FIRFilter {
public:

	FIRFilter(FloatType* taps) :
		CurrentIndex(size-1), LastPut(0)
	{
		for (unsigned int i = 0; i < size; ++i) {
			Kernel0[i] = taps[i];
			Signal[i] = 0.0;
			Signal[i + size] = 0.0;
		}

#ifdef USE_SIMD
		// Populate remaining kernel Phases:
		memcpy(1 + Kernel1, Kernel0, (size - 1)*sizeof(FloatType));
		Kernel1[0] = Kernel0[size - 1];
		memcpy(1 + Kernel2, Kernel1, (size - 1)*sizeof(FloatType));
		Kernel2[0] = Kernel1[size - 1];
		memcpy(1 + Kernel3, Kernel2, (size - 1)*sizeof(FloatType));
		Kernel3[0] = Kernel2[size - 1];
#endif

	}

	void put(FloatType value) { // Put signal in reverse order.
		Signal[CurrentIndex] = value;
		LastPut = CurrentIndex;
		if (CurrentIndex == 0) {
			CurrentIndex = size - 1; // Wrap
			memcpy(Signal + size, Signal, size*sizeof(FloatType)); // copy history to upper half of buffer
		}
		else
			--CurrentIndex;
	}

	void putZero() {
		Signal[CurrentIndex] = 0.0;
		if (CurrentIndex == 0) {
			CurrentIndex = size - 1; // Wrap
			memcpy(Signal + size, Signal, size*sizeof(FloatType)); // copy history to upper half of buffer
		}
		else
			--CurrentIndex;
	}

	FloatType get() {

#ifndef USE_SIMD
		FloatType output = 0.0;
		int index = CurrentIndex;
		for (int i = 0; i < size; ++i) {
			output += Signal[index] * Kernel0[i];
			index++;
		}
		return output;
#else
		// SIMD implementation: This only works with floats !
		// speed-up is around 3x on my system ...

		FloatType output = 0.0;
		FloatType* Kernel = Kernel0;
		int Index = (CurrentIndex >> 2) << 2; // make multiple-of-four
		int Phase = CurrentIndex & 3;
		
		// Part1 : Head
		// select proper Kernel phase and calculate first Block of 4:
		switch (Phase) {
		case 0:
			Kernel = Kernel0;
			// signal already aligned and ready to use
			output = Kernel[0] * Signal[Index] + Kernel[1] * Signal[Index + 1] + Kernel[2] * Signal[Index + 2] + Kernel[3] * Signal[Index + 3];
			break;
		case 1:
			Kernel = Kernel1;
			// signal starts at +1 : load first value from history (ie upper half of buffer)
			output = Kernel[0] * Signal[Index + size] + Kernel[1] * Signal[Index + 1] + Kernel[2] * Signal[Index + 2] + Kernel[3] * Signal[Index + 3];
			break;
		case 2:
			Kernel = Kernel2;
			// signal starts at +2 : load first and second values from history (ie upper half of buffer)
			output = Kernel[0] * Signal[Index + size] + Kernel[1] * Signal[Index + size + 1] + Kernel[2] * Signal[Index + 2] + Kernel[3] * Signal[Index + 3];
			break;
		case 3: 
			Kernel = Kernel3;
			// signal starts at +3 : load 1st, 2nd, 3rd values from history (ie upper half of buffer)
			output = Kernel[0] * Signal[Index + size] + Kernel[1] * Signal[Index + size + 1] + Kernel[2] * Signal[Index + size + 2] + Kernel[3] * Signal[Index + 3];
			break;
		}
		Index += 4;

		// Part 2: Body
		alignas(16) __m128 signal;	// SIMD Vector Registers for calculation
		alignas(16) __m128 kernel;
		alignas(16) __m128 product;
		alignas(16) __m128 accumulator = _mm_setzero_ps();

		for (int i = 4; i < (size >> 2) << 2; i += 4) {
			signal = _mm_load_ps(Signal + Index);
			kernel = _mm_load_ps(Kernel + i);
			product = _mm_mul_ps(signal, kernel);
			accumulator = _mm_add_ps(product, accumulator);
			Index += 4;
		}	

		output += accumulator.m128_f32[0] +
			accumulator.m128_f32[1] +
			accumulator.m128_f32[2] +
			accumulator.m128_f32[3];

		// Part 3: Tail
		for (int j = (size >> 2) << 2; j < size; ++j) {
			output += Signal[Index] * Kernel[j];
			++Index;
		}

		return output;

#endif // !USE_SIMD
	}

	FloatType LazyGet(int L) {	// Skips stuffed-zeros introduced by interpolation, by only calculating every Lth sample from LastPut
		FloatType output = 0.0;
		int Offset = LastPut - CurrentIndex;
		if (Offset < 0) { // Wrap condition
			Offset += size;
		}
	
		for (int i = Offset; i < size; i+=L) {
			output += Signal[i+ CurrentIndex] * Kernel0[i];
		}
		return output;
	}

private:
	alignas(16) FloatType Kernel0[size];

#ifdef USE_SIMD
	// Polyphase Filter Kernel table:
	alignas(16) FloatType Kernel1[size];
	alignas(16) FloatType Kernel2[size];
	alignas(16) FloatType Kernel3[size];
#endif
	alignas(16) FloatType Signal[size*2];	// Double-length signal buffer, to facilitate fast emulation of a circular buffer
	int CurrentIndex;
	int LastPut;
};

#ifdef USE_SIMD

// super-annoying Specializations for doubles (To-do: work out how to perfectly-forward the non-type template parameter 'size' ? ):

double FIRFilter<double, FILTERSIZE_MEDIUM>::get() {
	double output = 0.0;
	int index = CurrentIndex;
	for (int i = 0; i < FILTERSIZE_MEDIUM; ++i) {
		output += Signal[index] * Kernel0[i];
		index++;
	}
	return output;
}

double FIRFilter<double, FILTERSIZE_HUGE>::get() {
	double output = 0.0;
	int index = CurrentIndex;
	for (int i = 0; i < FILTERSIZE_HUGE; ++i) {
		output += Signal[index] * Kernel0[i];
		index++;
	}
	return output;
}

// actual SIMD implementations for doubles (not worth the effort - no faster than than naive):

//double FIRFilter<double, FILTERSIZE_MEDIUM>::get() {
//
//	// SIMD implementation: This only works with doubles !
//	// Processes two doubles at a time.
//
//	double output = 0.0;
//	double* Kernel;
//	int Index = (CurrentIndex >> 1) << 1; // make multiple-of-two
//	int Phase = CurrentIndex & 1;
//
//	// Part1 : Head
//	// select proper Kernel phase and calculate first Block of 2:
//	switch (Phase) {
//	case 0:
//		Kernel = Kernel0;
//		// signal already aligned and ready to use
//		output = Kernel[0] * Signal[Index] + Kernel[1] * Signal[Index + 1];
//		break;
//	case 1:
//		Kernel = Kernel1;
//		// signal starts at +1 : load first value from history (ie upper half of buffer)
//		output = Kernel[0] * Signal[Index + FILTERSIZE_MEDIUM] + Kernel[1] * Signal[Index + 1];
//		break;
//	}
//	Index += 2;
//
//	// Part 2: Body
//	alignas(16) __m128d signal;	// SIMD Vector Registers for calculation
//	alignas(16) __m128d kernel;
//	alignas(16) __m128d product;
//	alignas(16) __m128d accumulator = _mm_setzero_pd();
//
//	for (int i = 2; i < (FILTERSIZE_MEDIUM >> 1) << 1; i += 2) {
//		signal = _mm_load_pd(Signal + Index);
//		kernel = _mm_load_pd(Kernel + i);
//		product = _mm_mul_pd(signal, kernel);
//		accumulator = _mm_add_pd(product, accumulator);
//		Index += 2;
//	}
//
//	output += accumulator.m128d_f64[0] + accumulator.m128d_f64[1];
//
//	// Part 3: Tail
//	for (int j = (FILTERSIZE_MEDIUM >> 1) << 1; j < FILTERSIZE_MEDIUM; ++j) {
//		output += Signal[Index] * Kernel[j];
//		++Index;
//	}
//
//	return output;
//}
//
//double FIRFilter<double, FILTERSIZE_HUGE>::get() {
//
//	// SIMD implementation: This only works with doubles !
//	// Processes two doubles at a time.
//
//	double output = 0.0;
//	double* Kernel;
//	int Index = (CurrentIndex >> 1) << 1; // make multiple-of-two
//	int Phase = CurrentIndex & 1;
//
//	// Part1 : Head
//	// select proper Kernel phase and calculate first Block of 2:
//	switch (Phase) {
//	case 0:
//		Kernel = Kernel0;
//		// signal already aligned and ready to use
//		output = Kernel[0] * Signal[Index] + Kernel[1] * Signal[Index + 1];
//		break;
//	case 1:
//		Kernel = Kernel1;
//		// signal starts at +1 : load first value from history (ie upper half of buffer)
//		output = Kernel[0] * Signal[Index + FILTERSIZE_HUGE] + Kernel[1] * Signal[Index + 1];
//		break;
//	}
//	Index += 2;
//
//	// Part 2: Body
//	alignas(16) __m128d signal;	// SIMD Vector Registers for calculation
//	alignas(16) __m128d kernel;
//	alignas(16) __m128d product;
//	alignas(16) __m128d accumulator = _mm_setzero_pd();
//
//	for (int i = 2; i < (FILTERSIZE_HUGE >> 1) << 1; i += 2) {
//		signal = _mm_load_pd(Signal + Index);
//		kernel = _mm_load_pd(Kernel + i);
//		product = _mm_mul_pd(signal, kernel);
//		accumulator = _mm_add_pd(product, accumulator);
//		Index += 2;
//	}
//	
//	output += accumulator.m128d_f64[0] + accumulator.m128d_f64[1];
//
//	// Part 3: Tail
//	for (int j = (FILTERSIZE_HUGE >> 1) << 1; j < FILTERSIZE_HUGE; ++j) {
//		output += Signal[Index] * Kernel[j];
//		++Index;
//	}
//
//	return output;
//}

#endif // USE_SIMD

template<typename FloatType> bool makeLPF(FloatType* filter, int Length, FloatType transitionFreq, FloatType sampleRate)
{
	FloatType ft = transitionFreq / sampleRate; // normalised transition frequency
	assert(ft < 0.5);
	int halfLength = Length / 2;
	FloatType halfM = 0.5 * (Length - 1);

	if (halfLength & 1)
		filter[halfLength] = 2.0 * ft; // if length is odd, avoid divide-by-zero at centre-tap

	for (int n = 0; n<halfLength; ++n) {
		FloatType sinc = sin(2.0 * M_PI * ft * (n - halfM)) / (M_PI * (n - halfM));	// sinc function
		filter[Length - n - 1] = filter[n] = sinc;	// exploit symmetry
	}

	return true;
}

// This function converts a requested sidelobe height (in dB) to a value for the Beta parameter used in a Kaiser window:
template<typename FloatType> FloatType calcKaiserBeta(FloatType dB) 
{
	if(dB<21.0)
	{
		return 0;
	}
	else if ((dB >= 21.0) && (dB <= 50.0)) {
		return 0.5842 * pow((dB - 21), 0.4) + 0.07886 * (dB - 21);
	}
	else if (dB>50.0) {
		return 0.1102 * (dB - 8.7);
	}
}

// This function applies a Kaiser Window to an array of filter coefficients:
template<typename FloatType> bool applyKaiserWindow(FloatType* filter, int Length, FloatType Beta)
{
	for (int n = 0; n < Length; ++n) {
		filter[n] *= I0((2.0 * Beta / Length) * sqrt(n*(Length - n))) / I0(Beta); // simplified Kaiser Window Equation
	}
	return true;
}

template<typename FloatType> FloatType I0(FloatType z)
{	// 0th-order Modified Bessel function of the first kind
	FloatType result = 0.0;
	FloatType kfact = 1.0;
	for (int k = 0; k < 16; ++k) {
		if (k) kfact *= k;
		result += pow((pow(z, 2.0) / 4.0), k) / pow(kfact, 2.0);
	}
	return result;
}

#endif // FIRFFILTER_H_
