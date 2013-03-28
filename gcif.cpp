#include <iostream>
#include <vector>
using namespace std;

#include "Log.hpp"
#include "Clock.hpp"
#include "ImageMaskWriter.hpp"
#include "ImageMaskReader.hpp"
using namespace cat;

#include "lodepng.h"
#include "optionparser.h"


class Converter {
public:
	bool compress(const char *filename, const char *outfile) {
		vector<unsigned char> image;
		unsigned width, height;

		unsigned error = lodepng::decode(image, width, height, filename);

		CAT_INFO("main") << "Original image hash: " << hex << MurmurHash3::hash(&image[0], image.size());

		if (error) {
			CAT_WARN("main") << "Decoder error " << error << ": " << lodepng_error_text(error);
			return false;
		}

		if ((width & 7) | (height & 7)) {
			CAT_WARN("main") << "Image dimensions must be an even multiple of 8x8";
			return false;
		}

		// Generate ImageMask
		ImageMaskWriter imageMaskWriter;
		if (!imageMaskWriter.initFromRGBA(&image[0], width, height)) {
			CAT_WARN("main") << "Unable to generate image mask";
			return false;
		}

		ImageWriter writer;
		if (writer.init(width, height) != WE_OK) {
			CAT_WARN("main") << "Unable to initialize image writer";
			return false;
		}

		imageMaskWriter.write(writer);
		if (writer.finalizeAndWrite(outfile) != WE_OK) {
			CAT_WARN("main") << "Unable to finalize and write image mask";
			return false;
		}

		return true;
	}

	int _width, _height, _stride;
	int _writeRow;
	u32 *_image;

	int _sum, _rowLeft;
	bool _rowStarted;

	u32 *_row;
	int _bitOffset;
	bool _bitOn;
	int _lastSum;
	double _rleTime;

	bool decodeRLE(u8 *rle, int len) {
		double t0 = Clock::ref()->usec();

		if (len <= 0) {
			_rleTime += Clock::ref()->usec() - t0;
			return false;
		}

		u32 sum = _sum;
		bool rowStarted = _rowStarted;
		int rowLeft = _rowLeft;
		u32 *row = _row;
		const int stride = _stride;
		int bitOffset = _bitOffset;
		bool bitOn = _bitOn;

		for (int ii = 0; ii < len; ++ii) {
			u8 symbol = rle[ii];

			sum <<= 7;
			if CAT_UNLIKELY(symbol & 128) {
				sum |= symbol & 127;
			} else {
				sum |= symbol;

				// If has read row length yet,
				if (rowStarted) {
					int wordOffset = bitOffset >> 5;
					int newBitOffset = bitOffset + sum;
					int newOffset = newBitOffset >> 5;
					int shift = 31 - (newBitOffset & 31);

					// If at the first row,
					if CAT_UNLIKELY(_writeRow <= 0) {
						/*
						 * First row is handled specially:
						 *
						 * For each input X:
						 * 1. Write out X bits of current state
						 * 2. Flip the state
						 * 3. Then write out one bit of the new state
						 * When out of input, pad to the end with current state
						 *
						 * Demo:
						 *
						 * (1) 1 0 1 0 0 0 1 1 1
						 *     0 1 1 1 0 0 1 0 0
						 * {1, 0, 0, 2}
						 * --> 1 0 1 0 0 0 1 1 1
						 */

						// If previous state was 0,
						if (bitOn ^= 1) {
							// Fill bottom bits with 0s (do nothing)

							// For each intervening word and new one,
							for (int ii = wordOffset + 1; ii < newOffset; ++ii) {
								row[ii] = 0;
							}

							// Write a 1 at the new location
							row[newOffset] = 1 << shift;
						} else {
							u32 bitsUsedMask = 0xffffffff >> (bitOffset & 31);

							if (newOffset <= wordOffset) {
								row[newOffset] |= bitsUsedMask & (0xfffffffe << shift);
							} else {
								// Fill bottom bits with 1s
								row[wordOffset] |= bitsUsedMask;

								// For each intervening word,
								for (int ii = wordOffset + 1; ii < newOffset; ++ii) {
									row[ii] = 0xffffffff;
								}

								// Set 1s for new word, ending with a 0
								row[newOffset] = 0xfffffffe << shift;
							}
						}
					} else {
						/*
						 * 0011110100
						 * 0011001100
						 * {2,0,2,0}
						 * 0011110100
						 *
						 * 0111110100
						 * 1,0,0,0,0,1
						 * 0110110
						 *
						 * Same as first row except only flip on when we get X = 0
						 * And we will XOR with previous row
						 */

						//if (_writeRow > 120 && _writeRow < 200) cout << sum << ":" << bitOn << " ";

						// If previous state was toggled on,
						if (bitOn) {
							u32 bitsUsedMask = 0xffffffff >> (bitOffset & 31);

							if (newOffset <= wordOffset) {
								//cout << "S(" << row[newOffset] << "," << bitsUsedMask << "," << shift << ") ";
								row[newOffset] ^= bitsUsedMask & (0xfffffffe << shift);
								//cout << "S(" << row[newOffset] << "," << bitsUsedMask << "," << shift << ") ";
							} else {
								//cout << "M ";
								// Fill bottom bits with 1s
								row[wordOffset] ^= bitsUsedMask;

								// For each intervening word,
								for (int ii = wordOffset + 1; ii < newOffset; ++ii) {
									row[ii] ^= 0xffffffff;
								}

								// Set 1s for new word, ending with a 0
								row[newOffset] ^= (0xfffffffe << shift);
							}

							bitOn ^= 1;
							_lastSum = 0;
						} else {
							// Fill bottom bits with 0s (do nothing)

							//cout << "Z ";
							row[newOffset] ^= (1 << shift);

							if (sum == 0 && _lastSum) {
								bitOn ^= 1;
							}
							_lastSum = 1;
						}
					}

					bitOffset += sum + 1;

					// If just finished this row,
					if (--rowLeft <= 0) {
						int wordOffset = bitOffset >> 5;

						if CAT_LIKELY(_writeRow > 0) {
							// If last bit written was 1,
							if (bitOn) {
								// Fill bottom bits with 1s

								row[wordOffset] ^= 0xffffffff >> (bitOffset & 31);

								// For each remaining word,
								for (int ii = wordOffset + 1; ii < stride; ++ii) {
									row[ii] ^= 0xffffffff;
								}
							}
						} else {
							// If last bit written was 1,
							if (bitOn) {
								// Fill bottom bits with 1s
								row[wordOffset] |= 0xffffffff >> (bitOffset & 31);

								// For each remaining word,
								for (int ii = wordOffset + 1; ii < stride; ++ii) {
									row[ii] = 0xffffffff;
								}
							} else {
								// Fill bottom bits with 0s (do nothing)

								// For each remaining word,
								for (int ii = wordOffset + 1; ii < stride; ++ii) {
									row[ii] = 0;
								}
							}
						}

						//if (_writeRow > 120 && _writeRow < 200) cout << endl;

						if (++_writeRow >= _height) {
							// done!
							_rleTime += Clock::ref()->usec() - t0;
							return true;
						}

						rowStarted = false;
						row += stride;
					}
				} else {
					rowLeft = sum;

					// If row was empty,
					if (rowLeft == 0) {
						//if (_writeRow > 120 && _writeRow < 200) cout << "(empty)" << endl;
						// Decode as an exact copy of the row above it
						if (_writeRow > 0) {
							u32 *copy = row - stride;
							for (int ii = 0; ii < stride; ++ii) {
								row[ii] = copy[ii];
							}
						} else {
							for (int ii = 0; ii < stride; ++ii) {
								row[ii] = 0xffffffff;
							}
						}

						if (++_writeRow >= _height) {
							// done!
							_rleTime += Clock::ref()->usec() - t0;
							return true;
						}

						row += stride;
					} else {
						rowStarted = true;

						// Reset row decode state
						bitOn = false;
						bitOffset = 0;
						_lastSum = 0;

						// Setup first word
						if CAT_LIKELY(_writeRow > 0) {
							u32 *copy = row - stride;
							for (int ii = 0; ii < stride; ++ii) {
								row[ii] = copy[ii];
							}
						} else {
							row[0] = 0;
						}
					}
				}

				sum = 0;
			}
		}

		_bitOffset = bitOffset;
		_bitOn = bitOn;
		_row = row;
		_sum = sum;
		_rowStarted = rowStarted;
		_rowLeft = rowLeft;

		_rleTime += Clock::ref()->usec() - t0;
		return false;
	}

	bool decompress(const char *filename, const char *outfile) {
		bool success = false;

		ImageReader reader;

		if (RE_OK != reader.init(filename)) {
			CAT_WARN("main") << "Unable to read file";
			return false;
		}

		ImageMaskReader maskReader;
		if (RE_OK != maskReader.read(reader)) {
			CAT_WARN("main") << "Unable to read mask";
			return false;
		}
/*
		CAT_WARN("main") << "Writing output image file: " << outfile;
		// Convert to image:

		vector<unsigned char> output;
		u8 bits = 0, bitCount = 0;

		for (int ii = 0; ii < _height; ++ii) {
			for (int jj = 0; jj < _width; ++jj) {
				u32 set = (_image[ii * _stride + jj / 32] >> (31 - (jj & 31))) & 1;
				bits <<= 1;
				bits |= set;
				if (++bitCount >= 8) {
					output.push_back(bits);
					bits = 0;
					bitCount = 0;
				}
			}
		}

		lodepng_encode_file(outfile, (const unsigned char*)&output[0], _width, _height, LCT_GREY, 1);
*/
		return true;
	}
};


//// Command-line parameter parsing

enum  optionIndex { UNKNOWN, HELP, VERBOSE, SILENT, COMPRESS, DECOMPRESS, TEST };
const option::Descriptor usage[] =
{
  {UNKNOWN, 0,"" , ""    ,option::Arg::None, "USAGE: gcif_mono [options] [output file path]\n\n"
                                             "Options:" },
  {HELP,    0,"h", "help",option::Arg::None, "  --[h]elp  \tPrint usage and exit." },
  {VERBOSE,0,"v" , "verbose",option::Arg::None, "  --[v]erbose \tVerbose console output" },
  {SILENT,0,"s" , "silent",option::Arg::None, "  --[s]ilent \tNo console output (even on errors)" },
  {COMPRESS,0,"c" , "compress",option::Arg::Optional, "  --[c]ompress <input PNG file path> \tCompress the given .PNG image." },
  {DECOMPRESS,0,"d" , "decompress",option::Arg::Optional, "  --[d]ecompress <input GCI file path> \tDecompress the given .GCI image" },
  {TEST,0,"t" , "test",option::Arg::Optional, "  --[t]est <input PNG file path> \tTest compression to verify it is lossless" },
  {UNKNOWN, 0,"" ,  ""   ,option::Arg::None, "\nExamples:\n"
                                             "  gcif_mono -tv ./original.png\n"
                                             "  gcif_mono -c ./original.png test.gci\n"
                                             "  gcif_mono -d ./test.gci decoded.png" },
  {0,0,0,0,0,0}
};

int processParameters(option::Parser &parse, option::Option options[]) {
	if (parse.error()) {
		CAT_FATAL("main") << "Error parsing arguments [retcode:1]";
		return 1;
	}

	if (options[SILENT]) {
		Log::ref()->SetThreshold(LVL_SILENT);
	}

	if (options[VERBOSE]) {
		Log::ref()->SetThreshold(LVL_INANE);
	}

	if (options[COMPRESS]) {
		if (parse.nonOptionsCount() != 2) {
			CAT_WARN("main") << "Input error: Please provide input and output file paths";
		} else {
			const char *inFilePath = parse.nonOption(0);
			const char *outFilePath = parse.nonOption(1);
			Converter converter;

			if (!converter.compress(inFilePath, outFilePath)) {
				CAT_INFO("main") << "Error during conversion [retcode:2]";
				return 2;
			}

			return 0;
		}
	} else if (options[DECOMPRESS]) {
		if (parse.nonOptionsCount() != 2) {
			CAT_WARN("main") << "Input error: Please provide input and output file paths";
		} else {
			const char *inFilePath = parse.nonOption(0);
			const char *outFilePath = parse.nonOption(1);
			Converter converter;

			if (!converter.decompress(inFilePath, outFilePath)) {
				CAT_INFO("main") << "Error during conversion [retcode:3]";
				return 3;
			}

			return 0;
		}
	} else if (options[TEST]) {
		CAT_FATAL("main") << "TODO";
	}

	option::printUsage(std::cout, usage);
	return 0;
}


//// Entrypoint

int main(int argc, const char *argv[]) {

	Clock::ref()->OnInitialize();

	if (argc > 0) {
		--argc;
		++argv;
	}

	option::Stats  stats(usage, argc, argv);
	option::Option *options = new option::Option[stats.options_max];
	option::Option *buffer = new option::Option[stats.buffer_max];
	option::Parser parse(usage, argc, argv, options, buffer);

	int retval = processParameters(parse, options);

	delete []options;
	delete []buffer;

	Clock::ref()->OnFinalize();

	return retval;
}

