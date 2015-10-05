#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <stdint.h>

#include <gsl/gsl_statistics_uchar.h>
#include <gsl/gsl_sort_uchar.h>

#include "tiffio.h"

struct img {
	uint32_t x, y;
	uint32_t w, h;
	uint32_t *data;
} ;

struct img *img_load(const char* filename) {
	TIFF* tif = TIFFOpen(filename, "r");
	if (!tif)
		return NULL;

	struct img *img = malloc(sizeof(struct img));
	if(!img) {
		TIFFClose(tif);
		return NULL;
	}

	// retrieve width/height
	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &(img->w));
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &(img->h));

	// retrieve and compute offset
	float xpos, xres, ypos, yres;
	TIFFGetField(tif, TIFFTAG_XPOSITION,   &xpos);
	TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
	TIFFGetField(tif, TIFFTAG_YPOSITION,   &ypos);
	TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);

	img->x = (uint32)roundf(xpos*xres);
	img->y = (uint32)roundf(ypos*yres);

	uint32_t npixels = img->w * img->h;
	img->data = (uint32*) _TIFFmalloc(npixels * sizeof (uint32));

	if(img->data != NULL) {
		if(!TIFFReadRGBAImageOriented(tif, img->w, img->h, img->data, ORIENTATION_TOPLEFT, 0)) {
			_TIFFfree(img->data);
			free(img);
			TIFFClose(tif);
			return NULL;
		}
	}

	TIFFClose(tif);
	return img;
}

void img_free(struct img *img) {
	_TIFFfree(img->data);
	free(img);
}

int main(int argc, char* argv[]) {
	int n_images = argc-1;

	// load images
	struct img **images = malloc(n_images * sizeof(struct img *));

	if(!images) {
		free(images);
		fprintf(stderr, "Failed to allocate memory for image list.\n");
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "Loading %3d images...\n", n_images);
#pragma omp parallel for schedule(dynamic,1)
	for(int i=0; i<n_images; i++) {
		fprintf(stderr, "\t%3d/%-3d %s... ", i+1, n_images, argv[1+i]);
		images[i] = img_load(argv[1+i]);
		if(!images[i]) {
			fprintf(stderr, "failed.\n");
			exit(EXIT_FAILURE);
		} else {
			fprintf(stderr, "loaded: %5dx%-5d+%5d+%-5d.\n", images[i]->w, images[i]->h, images[i]->x, images[i]->y);
		}
	}

	// compute canvas size
	uint32_t canvas_w = 0, canvas_h = 0;
	for(int i=0; i<n_images; i++) {
		if(canvas_w < (images[i]->x+images[i]->w))
			canvas_w = images[i]->x + images[i]->w;
		if(canvas_h < (images[i]->y+images[i]->h))
			canvas_h = images[i]->y + images[i]->h;
	}

	fprintf(stderr, "Blending to a %5dx%5d final canvas...\n", canvas_w, canvas_h);
	uint32_t *canvas = malloc(canvas_w*canvas_h*sizeof(uint32_t));

	// compute median
	uint32_t *buffer = malloc(n_images*sizeof(uint32_t));
	for(uint32_t y=0; y<canvas_h; y++) {
		for(uint32_t x=0; x<canvas_w; x++) {
			int count=0;
			for(int i=0; i<n_images; i++) {
				struct img *img = images[i];
				if((x < img->x) || (y < img->y) || (x >= (img->x+img->w)) || (y >= (img->y+img->h)))
					continue;
				
				uint32_t px = img->data[(y-img->y)*img->w+(x-img->x)];
				if(TIFFGetA(px) == 0)
					continue;

				buffer[count] = px;
				count++;
			}
			gsl_sort_uchar(((uint8_t *)buffer),   4, count);
			gsl_sort_uchar(((uint8_t *)buffer)+1, 4, count);
			gsl_sort_uchar(((uint8_t *)buffer)+2, 4, count);
			gsl_sort_uchar(((uint8_t *)buffer)+3, 4, count);
			double med0 = gsl_stats_uchar_median_from_sorted_data(((uint8_t *)buffer),   4, count);
			double med1 = gsl_stats_uchar_median_from_sorted_data(((uint8_t *)buffer)+1, 4, count);
			double med2 = gsl_stats_uchar_median_from_sorted_data(((uint8_t *)buffer)+2, 4, count);
			double med3 = gsl_stats_uchar_median_from_sorted_data(((uint8_t *)buffer)+3, 4, count);
			uint32_t px = ((uint8_t)round(med0)) + (((uint8_t)round(med1))<<8) + (((uint8_t)round(med2))<<16) + (((uint8_t)round(med3))<<24);
			canvas[y*canvas_w + x] = px;
		}
	}

	// write output
	fprintf(stderr, "Writing output...\n");
	TIFF* output = TIFFOpen("out.tif", "w");

	const size_t SAMPLES_PER_PIXEL = 4;
	size_t bytes_per_line = canvas_w * SAMPLES_PER_PIXEL;

	// set image metadata
	TIFFSetField(output, TIFFTAG_IMAGEWIDTH,      canvas_w);
	TIFFSetField(output, TIFFTAG_IMAGELENGTH,     canvas_h);
	TIFFSetField(output, TIFFTAG_SAMPLESPERPIXEL, SAMPLES_PER_PIXEL);
	TIFFSetField(output, TIFFTAG_BITSPERSAMPLE,   8);
	TIFFSetField(output, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
	TIFFSetField(output, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
	TIFFSetField(output, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
	TIFFSetField(output, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(output, bytes_per_line));

	tdata_t buf = _TIFFmalloc(bytes_per_line);

	for(uint32_t y=0; y<canvas_h; y++) {
		memcpy(buf, &((uint8_t *)canvas)[(canvas_h-y-1)*bytes_per_line], bytes_per_line);
		TIFFWriteScanline(output, buf, y, 0);
	}
	
	TIFFClose(output);
	_TIFFfree(buf);

	// clean up
	fprintf(stderr, "Cleaning up...\n");
	free(buffer);
	free(canvas);

	for(int i=0; i<n_images; i++) {
		img_free(images[i]);
	}

	free(images);
	fprintf(stderr, "Done.\n");
}
