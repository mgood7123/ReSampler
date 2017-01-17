// Ditherer.h
//
// (C) J.Niemann, 2016, 2017
//
// defines Ditherer class, for adding tpdf dither to input samples
//
// signalBits is the number of bits of the target bitformat
// ditherBits is the number of bits of dither to add, and doesn't have to be an integer
// input and output samples are of type FloatType
//

#ifndef DITHERER_H
#define DITHERER_H 1

// configuration:
#define USE_IIR // if defined, use IIR Filter for noise shaping, otherwise use FIR. 
#define DITHER_TOPOLOGY 1
//#define TEST_FILTER
#define DITHER_USE_SATURATION  // restrict output amplitude to +/- 0.999 (guards against excessive dither levels causing clipping)
// --- //

#define FIR_NOISE_SHAPING_FILTER_SIZE 9

#include <cmath>
#include "biquad.h"
#include <random>

template<typename FloatType>
class Ditherer  
{
public:
	unsigned int signalBits;
	FloatType ditherBits;

	// Auto-Blanking parameters:
	bool bAutoBlankingEnabled; 
	FloatType autoBlankLevelThreshold;				// input signals below this threshold are considered zero
	FloatType autoBlankTimeThreshold;				// number of zero samples before activating blanking
	const FloatType autoBlankDecayFactor = (FloatType)0.9995;	// dither level will decrease by this factor for each sample when blanking is active

// Constructor:

	Ditherer(unsigned int signalBits, FloatType ditherBits, bool bAutoBlankingEnabled, int seed) : 
		signalBits(signalBits), 
		ditherBits(ditherBits),
		bAutoBlankingEnabled(bAutoBlankingEnabled),
		seed(seed),
		Z1(0),
		Z2(0)

#ifndef USE_IIR
		,FIR((FloatType*)FIRNoiseShapeCoeffs, FIR_NOISE_SHAPING_FILTER_SIZE)
#endif
		,randGenerator(seed)		// initialize (seed) RNG
		,dist(0,randMax)		// set the range of the random number distribution
	{

#ifdef USE_IIR
		//// IIR-related stuff:
		if (ditherBits < 1.5)
		{
			// IIR noise-shaping filter (2 biquads) - flatter response; more energy in spectrum
			f1.setCoeffs(0.798141839881378,
				-0.7040563852194521,
				0.15341541599754416,
				0.3060312586301247,
				0.02511886431509577);

			f2.setCoeffs(0.5,
				-0.7215722413008345,
				0.23235922079486643,
				-1.5531272249269004,
				0.7943282347242815);
		}
		else
		{	
			// IIR noise-shaping filter (2 biquads)
			f1.setCoeffs(0.1872346691747817,
				-0.1651633303505913,
				0.03598944852318585,
				1.2861600144545022,
				0.49000000000000016);

			f2.setCoeffs(0.5,
				-0.7215722413008345,
				0.23235922079486643,
				-1.2511963408503206,
				0.5328999999999999);
		}

		// super-slick HPF (1 biquad):
		// f1.setCoeffs(0.008978326844454853, -0.017956653688909707, 0.008978326844454853, 1.6865949922346122, 0.7224999999999999);
		
#else 
		// FIR-related stuff:
		currentIndex = FIR_NOISE_SHAPING_FILTER_SIZE - 1;
		memset(noise, 0, FIR_NOISE_SHAPING_FILTER_SIZE * sizeof(FloatType));

#endif // USE_IIR	
		signalMagnitude = static_cast<FloatType>((1 << (signalBits - 1)) - 1); // note the -1 : match 32767 scaling factor for 16 bit !
		reciprocalSignalMagnitude = 1.0 / signalMagnitude; // value of LSB in target format
		maxDitherScaleFactor = (pow(2,ditherBits-1) * reciprocalSignalMagnitude) / randMax;
		oldRandom = newRandom = 0;

		if (bAutoBlankingEnabled) {	// initial state: silence
			ditherScaleFactor = 0.0;
		}
		else {	// initial state: dithering
			ditherScaleFactor = maxDitherScaleFactor; 
		}

		autoBlankLevelThreshold = 1.0 / pow(2, 32); // 1 LSB of 32-bit digital
		autoBlankTimeThreshold = 30000; // number of zero samples before activating autoblank
		autoBlankDecayCutoff = 0.7 * reciprocalSignalMagnitude / randMax;
		zeroCount = 0;
		
	} // Ends Constructor 

#if (DITHER_TOPOLOGY == 1)

// The Dither function ///////////////////////////////////////////////////////

// 1. Ditherer Topology:
//
//          tpdfNoise ------>[filter]
//                               |
//                    preDither  |    +-----> preQuantize
//                        ^      v    |
//   inSample ----->+( )--+---->(+)---+--->[Q]-->-+--> postQuantize
//                    -   |                       |
//                    ^   +-------------->-( )+<--+
//                    |                     |
//                    +-------[z^-1]---<----+
//                    |  1.00               |
//                    +-------[z^-2]---<----+
//                      -0.043
//
//


FloatType Dither(FloatType inSample) {

	// Auto-Blanking
	if (bAutoBlankingEnabled) {
		if (abs(inSample) < autoBlankLevelThreshold) {
			++zeroCount;
			if (zeroCount > autoBlankTimeThreshold) {
				ditherScaleFactor *= autoBlankDecayFactor; // decay
				if (ditherScaleFactor < autoBlankDecayCutoff)
					ditherScaleFactor = 0.0; // decay cutoff
			}
		}
		else {
			zeroCount = 0; // reset
			ditherScaleFactor = maxDitherScaleFactor; // restore
		}
	} // ends auto-blanking

	newRandom = dist(randGenerator);
	//oldRandom = dist(randGenerator);
	FloatType tpdfNoise = static_cast<FloatType>(newRandom - oldRandom);
	oldRandom = newRandom;
	FloatType preDither = inSample - Z1 + Z2*0.043;

#ifdef USE_IIR
	// IIR Noise Shaping:
	FloatType shapedNoise = ditherScaleFactor * f2.filter(f1.filter(tpdfNoise)); // filter the triangular noise with two cascaded biQuads 																				 
#else
	// FIR Noise Shaping:
	
	// put tpdf noise into history buffer (noise goes in "backwards"):
	noise[currentIndex--] = ditherScaleFactor * tpdfNoise;
	if (currentIndex < 0) {
		currentIndex = FIR_NOISE_SHAPING_FILTER_SIZE - 1;
	}

	// get result from FIR:
	FloatType shapedNoise = 0.0;
	int index = currentIndex;
	for (int i = 0; i < FIR_NOISE_SHAPING_FILTER_SIZE; ++i) {
		if (++index == FIR_NOISE_SHAPING_FILTER_SIZE) {
			index = 0;
		} 
		shapedNoise += noise[index] * FIRNoiseShapeCoeffs[i];
	}
	
#endif

#ifdef TEST_FILTER
	return shapedNoise; // (Output Only Filtered Noise - discard signal)
#endif

	// Calculate the quantization error. This needs to exactly model the behavior of the I/O library writing samples to outfile, 
	// otherwise nasty quantization distortion will result.
	FloatType preQuantize = preDither + shapedNoise;
	FloatType postQuantize = reciprocalSignalMagnitude * round(signalMagnitude * preQuantize); // quantize
	Z2 = Z1;
	Z1 = postQuantize - preDither; // calculate error 
	

#ifdef DITHER_USE_SATURATION
	return 0.5*(fabs(postQuantize + 1.0) - fabs(postQuantize - 1.0)); // branchless clipping - restrict to +/- 1.0 (0.0000 dB)
#endif
	return postQuantize;
} // ends function: Dither()

#endif //(DITHER_TOPOLOGY == 1)

#if (DITHER_TOPOLOGY == 2)

// The Dither function  - Topology #2 ///////////////////////////////////////////////////////

// 2. Ditherer Topology:
//
//							 tpdfNoise
//                               |
//                    preDither  |
//                         ^     |   +----------> preQuantize
//                         |     v   |               
//   inSample ----->+( )---+--->(+)--+->[Q]-->--+------> postQuantize
//                    -    |                    |
//                    ^    +---------->-( )+<---+
//                    |                  |               
//                 [filter]              | 
//                    |                  v
//                    +---<---[z^-1]-----+
//

FloatType Dither(FloatType inSample) {

	// Auto-Blanking
	if (bAutoBlankingEnabled) {
		if (abs(inSample) < autoBlankLevelThreshold) {
			++zeroCount;
			if (zeroCount > autoBlankTimeThreshold) {
				ditherScaleFactor *= autoBlankDecayFactor; // decay
				if (ditherScaleFactor < autoBlankDecayCutoff)
					ditherScaleFactor = 0.0; // decay cutoff
			}
		}
		else {
			zeroCount = 0; // reset
			ditherScaleFactor = maxDitherScaleFactor; // restore
		}
	} // ends auto-blanking

	newRandom = dist(randGenerator);
	FloatType tpdfNoise = ditherScaleFactor * static_cast<FloatType>(newRandom - oldRandom);
	oldRandom = newRandom;

#ifdef USE_IIR
	// IIR Filter:

	#ifdef TEST_FILTER
	return f2.filter(f1.filter(tpdfNoise)); // (Output Only Filtered Noise - discard signal)
	#endif

	FloatType preDither = inSample - f2.filter(f1.filter(Z1));
#else
	// FIR Filter:

	#ifdef TEST_FILTER
		Z1 = tpdfNoise;
	#endif

	// put Z1 into history buffer (goes in "backwards"):
	noise[currentIndex--] = Z1;
	if (currentIndex < 0) {
		currentIndex = FIR_NOISE_SHAPING_FILTER_SIZE - 1;
	}

	// get result from FIR:
	FloatType filterOutput = 0.0;
	int index = currentIndex;
	for (int i = 0; i < FIR_NOISE_SHAPING_FILTER_SIZE; ++i) {
		if (++index == FIR_NOISE_SHAPING_FILTER_SIZE) {
			index = 0;
		}
		filterOutput += noise[index] * FIRNoiseShapeCoeffs[i];
	}

	#ifdef TEST_FILTER
		return filterOutput;
	#endif

	FloatType preDither = inSample - filterOutput;

#endif //!USE_IIR

	FloatType preQuantize, postQuantize;
	preQuantize = preDither + tpdfNoise;
	postQuantize = reciprocalSignalMagnitude * round(signalMagnitude * preQuantize); // quantize
	Z1 = postQuantize - preDither;		

#ifdef DITHER_USE_SATURATION
	return (0.5*(fabs(postQuantize + 1.0) - fabs(postQuantize - 1.0))); // branchless clipping (restrict to +/- 1.0)
#else
	return postQuantize;
#endif

} // ends function: Dither()

#endif //(DITHER_TOPOLOGY == 2)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

private:
	int oldRandom, newRandom;
	int seed;
	FloatType Z1,Z2;				// last Quantization error
	FloatType signalMagnitude;	// maximum integral value for signal target bit depth (for quantizing) 
	FloatType reciprocalSignalMagnitude; // for normalizing quantized signal back to +/- 1.0 
	FloatType maxDitherScaleFactor, ditherScaleFactor;	// maximum integral value for dither target bit depth
	__int64 zeroCount; // number of consecutive zeroes in input;
	FloatType autoBlankDecayCutoff;	// threshold at which ditherScaleFactor is set to zero during active blanking
	std::mt19937 randGenerator; // Mersenne Twister - one of the best random number algorithms available
	std::uniform_int_distribution<int> dist; // random number distribution
	static const int randMax = 32767; /*16777215; // 2^24 - 1 */
	
#ifdef USE_IIR
	// IIR Filter-related stuff:
	Biquad<double> f1;
	Biquad<double> f2;
#else	
	// FIR Filter-related stuff:
	FIRFilter<FloatType> FIR;
	int currentIndex;
	FloatType noise[FIR_NOISE_SHAPING_FILTER_SIZE]; // (circular) buffer for noise history
#endif
};

const double FIRNoiseShapeCoeffs[FIR_NOISE_SHAPING_FILTER_SIZE] = {

	/*
	// 11-tap:
	-0.014702063883960252,
	-0.0010319367876352055,
	0.06696663418581869,
	0.0010013618187379699,
	-0.30273448956854543,
	0.49822670684302905,
	-0.30273448956854543,
	0.0010013618187379699,
	0.06696663418581869,
	-0.0010319367876351854,
	-0.014702063883960252
	*/

	// Psychoacoustically Optimal Noise Shaping:
	// this filter is the "F-Weighted" noise filter described by Wannamaker.
	// It is designed to produce minimum audibility:
	
	/*
	// 3-tap:
		1.623,
	-0.982,
	0.109
	*/

	
	// 9-tap:
	2.412,
	-3.370,
	3.937,
	-4.174,
	3.353,
	-2.205,
	1.281,
	-0.569,
	0.0847
	
	/*
	// 9-tap normalized to 0dB peak (x0.04):
	0.09648,
	- 0.13480,
	0.15748,
	- 0.16696,
	0.13412,
	- 0.08820,
	0.05124,
	- 0.02276,
	0.00339
	*/
	
	//0.00339,  // reversed, normalized to 0dB peak (x0.04)
	//- 0.02276,
	//0.05124,
	//- 0.08820,
	//0.13412,
	//- 0.16696,
	//0.15748,
	//- 0.13480,
	//0.09648

	/*
	//24-tap (needs normalization):
	2.391510,
	-3.284444,
	3.679506,
	-3.635044,
	2.524185,
	-1.146701,
	0.115354,
	0.513745,
	-0.749277,
	0.512386,
	-0.188997,
	-0.043705,
	0.149843,
	-0.151186,
	0.076302,
	-0.012070,
	-0.021127,
	0.025232,
	-0.016121,
	0.004453,
	0.000876,
	-0.001799,
	0.000774,
	-0.000128
	*/
	// some previous filter attempts: 
	
	/*
	// High-Shibata 44k (20 taps)
		3.0259189605712890625, -6.0268716812133789062,   9.195003509521484375,
		-11.824929237365722656, 12.767142295837402344, -11.917946815490722656,
		9.1739168167114257812,  -5.3712320327758789062, 1.1393624544143676758,
		2.4484779834747314453,  -4.9719839096069335938,   6.0392003059387207031,
		-5.9359521865844726562,  4.903278350830078125,   -3.5527443885803222656,
		2.1909697055816650391, -1.1672389507293701172,  0.4903914332389831543,
		-0.16519790887832641602,  0.023217858746647834778
	*/

};


// *Psychoacoustically Optimal Noise Shaping
// Robert. A. Wannamaker
// Journal of the Audio Engineering Society 40(7 / 8) : 611 - 620 � July 1992

#endif // !DITHERER_H