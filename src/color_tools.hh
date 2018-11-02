/*
 * Copyright (C) 2015-2018 Dubalu LLC. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <cmath>             // for std::fmod


static inline void
hsv2rgb(
	// input:
	double hue,        // angle in degrees between 0 and 360
	double saturation, // a fraction between 0 and 1
	double value,      // a fraction between 0 and 1
	// output:
	double& red,       // a fraction between 0 and 1
	double& green,     // a fraction between 0 and 1
	double& blue       // a fraction between 0 and 1
) {
	if (saturation <= 0.0) {
		red = value;
		green = value;
		blue = value;
		return;
	}

	if (hue >= 360.0) {
		hue = std::fmod(hue, 360.0);
	}

	hue /= 60.0;
	auto i = static_cast<long>(hue);
	auto f = hue - i;
	auto p = value * (1.0 - saturation);
	auto q = value * (1.0 - (saturation * f));
	auto t = value * (1.0 - (saturation * (1.0 - f)));

	switch (i) {
		case 0:
			red = value;
			green = t;
			blue = p;
			break;
		case 1:
			red = q;
			green = value;
			blue = p;
			break;
		case 2:
			red = p;
			green = value;
			blue = t;
			break;
		case 3:
			red = p;
			green = q;
			blue = value;
			break;
		case 4:
			red = t;
			green = p;
			blue = value;
			break;
		case 5:
		default:
			red = value;
			green = p;
			blue = q;
			break;
	}
	return;
}
