/***************************************************************************\
*                                                                           *
* Copyright 2012 Michael Moon                                               *
*                                                                           *
*                                                                           *
* This program is free software: you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation, either version 3 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
* This program is distributed in the hope that it will be useful,           *
* but WITHOUT ANY WARRANTY; without even the implied warranty of            *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
* GNU General Public License for more details.                              *
*                                                                           *
* You should have received a copy of the GNU General Public License         *
* along with this program.  If not, see <http://www.gnu.org/licenses/>.     *
*                                                                           *
*                                                                           *
* This program lives at http://github.com/triffid/gcodeview and Author can  *
* be contacted via that site                                                *
*                                                                           *
\***************************************************************************/


#ifndef	_GNU_SOURCE
#define		_GNU_SOURCE
#endif

/* #include	<features.h> */

#include	<stdlib.h>
#include	<stdint.h>
#include    <stdio.h>
#include	<share.h>


/*
#include	<sys/mman.h>
#include	<sys/stat.h>
#include	<sys/types.h>
*/
#include <sys/types.h>
#include <sys/stat.h>

#include	<math.h>
#include	<errno.h>
#include	<string.h>
#include	<fcntl.h>
/*#include	<getopt.h>*/

#include "SDL\SDL.h"
/* #include	<FTGL/ftgl.h> */
/* #include	<fontconfig/fontconfig.h>*/

#define		bool uint8_t
#define		true	255
#define		false	0

#define		OPENGL

#ifdef	OPENGL
	#include	"SDL\SDL_opengl.h"
	float transX, transY, transZ;
#else
	#include	"SDL\SDL_gfxPrimitives.h"
	float viewPortL, viewPortR, viewPortT, viewPortB;
#endif


// main loop fall-through flag
bool Running;

// getopt stuff
static const char *optString = "l:w:h?";
/*static const struct option longOpts[] = {
	{ "layer", required_argument, NULL, 'l' },
	{ "width", required_argument, NULL, 'w' },
	{ 0      , 0                , 0   , 0   }
};*/

// GCODE file related stuff
int filesz;
char* gcodefile;
float extrusionWidth = 0.3;
int layerCount;
size_t layerSize;
float linewords[26];

#define	LMASK(l) (1<<((l & ~0x20) - 'A'))
#define	SEEN(c) ((seen & LMASK(c)) != 0)


#define	LD_LISTGENERATED 1
typedef struct {
	char*	index;
	int		size;
	float	height;
	uint8_t	flags;
} layerData;
layerData* layer;

// FTGL stuff for drawing text
/* FTGLfont* font = NULL;*/
char *msgbuf;

// SDL window and GL Viewport
SDL_Surface* Surf_Display;
int Surf_width;
int Surf_height;

// Current View settings
int layerVelocity;
int currentLayer;
float zoomFactor;

int glListsBase = 0;

// SDL Events Interface
#define	KMM_LSHIFT 1
#define KMM_RSHIFT 2
#define	KMM_CTRL   4
#define	KMM_ALT    8
int keymodifiermask;

#define	TIMER_KEYREPEAT 1
#define	TIMER_DRAGRENDER 2
SDL_TimerID timerKeyRepeat = NULL;
SDL_TimerID timerDragRender = NULL;
float gXmouseDown = 0.0, gYmouseDown = 0.0;

/***************************************************************************\
*                                                                           *
* Utility Functions                                                         *
*                                                                           *
\***************************************************************************/

void display_usage() {
	printf("\n");
	printf("USAGE: gcodeview [-w|--width width] [-l|--layer layer] <file.gcode>\n");
	printf("\n");
	printf("\twidth: Extrusion Width used to draw lines\n");
	printf("\tlayer: Render this layer first\n");
	printf("\n");
	printf("Color Key:\n");
	printf("\n");
	printf("\tBlack: Extrusion move at current layer height\n");
	printf("\tGreen: Travel move at current layer height\n");
	printf("\tRed:   Travel move at different layer height (ie hop/z-lift)\n");
	printf("\n");
	exit(0);
}

float minf(float a, float b) {
	if (a < b)
		return a;
	return b;
}

float maxf(float a, float b) {
	if (a > b)
		return a;
	return b;
}

float linint(float value, float oldmin, float oldmax, float newmin, float newmax) {
	return (value - oldmin) * (newmax - newmin) / (oldmax - oldmin) + newmin;
}

void die(char* call, char* data) {
	int errsv = errno;
	fprintf(stderr, "%s%s failed: %s\n", call, data, strerror(errsv));
	exit(1);
}

Uint32 timerCallback(Uint32 interval, void* param) {
	SDL_Event e;
	e.type = SDL_USEREVENT;
	e.user.code = (int) param;
	e.user.data1 = 0;
	e.user.data2 = 0;

	SDL_PushEvent(&e);

	return 20;
}

/***************************************************************************\
*                                                                           *
* Read a single line of GCODE, extracting which words are present and their *
* values                                                                    *
*                                                                           *
\***************************************************************************/

uint32_t scanline(char *line, int length, float *words, char **end) {
	int i = 0;
	uint32_t seen = 0;

	#define	COMMENT_SEMICOLON 1
	#define	COMMENT_PARENTHESIS 2
	int comment = 0;
	while (i < length) {
		char c = line[i];
		if (c == 13 || c == 10) // carriage return or new line
		{
			//printf(".. found a line...%c\n", line[i-1]);
			*end = &line[i + 1];
			return seen;
		}
		if ((comment & COMMENT_SEMICOLON) == 0) 
		{
			//printf(".. found a COMMENT_SEMICOLON\n");
			if (c == ';')
			{
				comment |= COMMENT_SEMICOLON;
			}
			else if (c == '(')
			{
				comment |= COMMENT_PARENTHESIS;
			}
			else if (c == ')')
			{
				comment &= ~COMMENT_PARENTHESIS;
			}
			else if (comment == 0) 
			{
				//printf(".. found a comment == 0\n");
				if (c >= 'a' && c <= 'z')
					c &= ~0x20;
				if (c >= 'A' && c <= 'Z') 
				{
					char *e = NULL;
					float v = strtod(&line[i + 1], &e);/*strtof(&line[i + 1], &e);*/
					//printf(".. found a c >= A and line= %c\n", line[i + 1]);
					if (e > &line[i + 1]) 
					{
						//printf(".. found a e > &line\n");
						seen |= LMASK(c);
						words[c - 'A'] = v;
						i = e - line - 1;
					}
				}
			}
		}
		i++;
	}
	*end = &line[i];
	return seen;
}

/***************************************************************************\
*                                                                           *
* Draw a thick line (QUAD) given gcode coordinates, width and RGBA          *
*                                                                           *
\***************************************************************************/

void gline(float x1, float y1, float x2, float y2, float width, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	#ifdef	OPENGL
		glColor4f(((float) r) / 255.0, ((float) g) / 255.0, ((float) b) / 255.0, ((float) a) / 255.0);
		//   c1x,c1y
		//  0,0......
		//   c4x,c4y  ........       c2x,c2y
		//                    ........ px,py
		//                       c3x,c3y
		float c1x, c1y, c1l, c2x, c2y, c2l, c3x, c3y, c3l, c4x, c4y, c4l;

		float px = x2 - x1;
		float py = y2 - y1;

		c1x = -py;
		c1y = px;
		c1l = hypotf(c1x, c1y);
		c1x = (c1x * width / c1l / 2.0) + x1;
		c1y = (c1y * width / c1l / 2.0) + y1;

		c2x = -py;
		c2y = px;
		c2l = hypotf(c2x, c2y);
		c2x = (c2x * width / c2l / 2.0) + px + x1;
		c2y = (c2y * width / c2l / 2.0) + py + y1;

		c3x = py;
		c3y = -px;
		c3l = hypotf(c3x, c3y);
		c3x = (c3x * width / c3l / 2.0) + px + x1;
		c3y = (c3y * width / c3l / 2.0) + py + y1;

		c4x = py;
		c4y = -px;
		c4l = hypotf(c4x, c4y);
		c4x = (c4x * width / c4l / 2.0) + x1;
		c4y = (c4y * width / c4l / 2.0) + y1;

		if (width == 4.0)
			printf("LINE: [%3.0f,%3.0f]->[%3.0f,%3.0f]->[%3.0f,%3.0f]->[%3.0f,%3.0f]\n", c1x, c1y, c2x, c2y, c3x, c3y, c4x, c4y);

		glVertex2f(c1x, c1y);
		glVertex2f(c2x, c2y);
		glVertex2f(c3x, c3y);
		glVertex2f(c4x, c4y);
	#else
	thickLineRGBA(Surf_Display,
		(x1 - viewPortL) * zoomFactor,
		(viewPortB - y1) * zoomFactor,
		(x2 - viewPortL) * zoomFactor,
		(viewPortB - y2) * zoomFactor,
		mind(maxd(width * zoomFactor, 1), 2),
		r, g, b, a
		);
	#endif
}

/***************************************************************************\
*                                                                           *
* create the quads for a layer, no wrappers                                 *
*                                                                           *
\***************************************************************************/
#ifdef WIN32
    #ifndef NAN
        static const unsigned long __nan[2] = {0xffffffff, 0x7fffffff};
        #define NAN (*(const float *) __nan)
    #endif
	#define isnan(x) ((x) != (x))
#endif

void render_layer(int clayer, float alpha) {
	char *s = layer[clayer].index;
	char *e = layer[clayer].index + layer[clayer].size;
	float G = NAN, X = NAN, Y = NAN, E = NAN, Z = NAN, lastX = NAN, lastY = NAN, lastE = NAN;
	uint32_t seen = 0;

	for (X = 0; X < 201.0; X += 10.0) {
		gline(X, 0, X, 200, ((((int) X) % 50) == 0)?1:0.2, 0, 0, 0, 16);
		gline(0, X, 200, X, ((((int) X) % 50) == 0)?1:0.2, 0, 0, 0, 16);
	}

	while (s < e) {
		seen = scanline(s, e - s, linewords, &s);
		if (SEEN('G') && (SEEN('X') || SEEN('Y'))) {
			if (linewords['G' - 'A'] == 0.0 || linewords['G' - 'A'] == 1.0) {
				G = linewords['G' - 'A'];
				X = linewords['X' - 'A'];
				Y = linewords['Y' - 'A'];
				Z = linewords['Z' - 'A'];
				E = linewords['E' - 'A'];
				// draw
				uint8_t r = 0, g = 0, b = 0, a = 224;
				if (isnan(lastX))
					lastX = X;
				if (isnan(lastY))
					lastY = Y;
				if (isnan(lastE))
					lastE = E;
				if (SEEN('E') && (E > lastE)) {
					r = 0;
					g = 0;
					b = 0;
					a = 224;
				}
				else if (Z != layer[clayer].height) {
					if (Z > layer[clayer].height) {
						r = 224;
						g = 64;
						b = 64;
					}
					else {
						r = 128;
						g = 0;
						b = 128;
					}
					a = 160;
				}
				else {
					r = 0;
					g = 128;
					b = 64;
					a = 160;
				}
				if ((lastX != X || lastY != Y) && !isnan(X) && !isnan(Y) && lastX <= 200.0)
					gline(lastX, lastY, X, Y, extrusionWidth, r, g, b, a * alpha);
			}
			seen = 0;
			lastX = X;
			lastY = Y;
			lastE = E;
		}
	}
}

/***************************************************************************\
*                                                                           *
* Update the OpenGL display with the current layer                          *
*                                                                           *
\***************************************************************************/

void render() {
	#ifdef	OPENGL
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glLoadIdentity();
		glPushMatrix();
		glScalef(zoomFactor, zoomFactor, 0.0);
		glTranslatef(-transX, -transY, 0.0);
		if (layer[currentLayer].flags & LD_LISTGENERATED) {
			glCallList(glListsBase + currentLayer);
		}
		else {
			glNewList(glListsBase + currentLayer, GL_COMPILE_AND_EXECUTE);
			glBegin(GL_QUADS);
	#else
		uint32_t yellow;

		yellow = SDL_MapRGB(Surf_Display->format, 224, 224, 128);

		SDL_LockSurface(Surf_Display);
		SDL_FillRect(Surf_Display, NULL, yellow);
		int lines = 0;
	#endif

	#define SHADOW_LAYERS 3
	#define	SHADOW_ALPHA  0.2
	for (int i = SHADOW_LAYERS; i >= 1; i--) {
		if (currentLayer - i > 0)
			render_layer(currentLayer - i, SHADOW_ALPHA - (i - 1) * (SHADOW_ALPHA / SHADOW_LAYERS));
	}
	render_layer(currentLayer, 1.0);

	#ifdef	OPENGL
			glEnd();
			glEndList();
			layer[currentLayer].flags |= LD_LISTGENERATED;
		}
			glPopMatrix();
			glPushMatrix();
				glTranslatef(0.0, 200.0 - (20.0 * 0.3), 0.0);
				glScalef(0.3, 0.3, 1.0);
				/*ftglSetFontFaceSize(font, 20, 20);
				ftglRenderFont(font, msgbuf, FTGL_RENDER_ALL);*/
			glPopMatrix();
			glFlush();
			glFinish();
		SDL_GL_SwapBuffers();
		glFinish();
	#else
		SDL_UnlockSurface(Surf_Display);

		SDL_Flip(Surf_Display);
	#endif
}

/***************************************************************************\
*                                                                           *
* Resize the display                                                        *
*                                                                           *
* Includes refreshing the OpenGL Context                                    *
*                                                                           *
\***************************************************************************/

void resize(int w, int h) {
	Surf_width = w;
	Surf_height = h;
	#ifdef	OPENGL
		int dim;
		if (w > h)
			dim = h;
		else
			dim = w;

		if (glListsBase)
			glDeleteLists(glListsBase, layerCount);

		if (Surf_Display != NULL)
			SDL_FreeSurface(Surf_Display);

		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		Surf_Display = SDL_SetVideoMode(Surf_width, Surf_height, 32, SDL_HWSURFACE | SDL_RESIZABLE | SDL_OPENGL);
		w = Surf_Display->w; h = Surf_Display->h;
		glViewport(0, 0, w, h);
		glClearColor(0.8, 0.8, 0.5, 0.5);
		glClearDepth(1.0f);
		glShadeModel(GL_SMOOTH);
		glEnable(GL_BLEND);
		glEnable(GL_POLYGON_SMOOTH);
		glEnable(GL_LINE_SMOOTH);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		/*glOrtho(0, 200 * w / dim, 0, 200 * h / dim, 0, 1);*/
		glOrtho(0, 200 * w / dim, 0, 200 * h / dim, 0, 1);
		glDisable(GL_DEPTH_TEST);
		/*glEnable(GL_DEPTH_TEST);*/
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glListsBase = glGenLists(layerCount);
		for (int i = 0; i < layerCount; layer[i++].flags = 0);
	#else
		Surf_Display = SDL_SetVideoMode(Surf_width, Surf_height, 32, SDL_HWSURFACE | SDL_floatBUF | SDL_RESIZABLE);
	#endif
	if (Surf_Display == NULL) {
		SDL_FreeSurface(Surf_Display);
		SDL_Quit();
		die("SDL resize", "");
	}
	render(); // redraw whole window
}

/***************************************************************************\
*                                                                           *
* Simple function to change current layer                                   *
*                                                                           *
\***************************************************************************/

void drawLayer(int layerNum) {
	//snprintf(msgbuf, 256, "Layer %3d: %gmm", layerNum, layer[layerNum].height);
//	printf("Drawing layer %3d (%5.2f)\n", layerNum, layer[layerNum].height);
	currentLayer = layerNum;
	render();
}

/***************************************************************************\
*                                                                           *
* Read lines from GCODE input file                                          *
* Earmark the start of each layer in the file so we can find it quickly     *
*                                                                           *
\***************************************************************************/

void scanLines() {
	int l = 0;

	printf("Indexing lines... ");
	
	layerCount = 0;
	// preallocate for 128 layers, we double the size later if it's not enough
	layerSize = (128 * sizeof(layerData));
	layer = (layerData *) malloc(layerSize);

	char *end;
	uint32_t seen;
	float G, Z, lastZ = 0.0, E;

	int ZstackIndex = 0;
	struct {
		char *start;
		float Z;
	} Zstack[8];

	printf("....filesz... %ld", filesz);

	while (l < filesz) {
		seen = scanline(&gcodefile[l], filesz - l, linewords, &end);
		
		G = linewords['G' - 'A'];
		Z = linewords['Z' - 'A'];
		E = linewords['E' - 'A'];
		
		if (((seen & LMASK('G')) != 0) && (G == 0.0 || G == 1.0)) 
		{
			//printf("seen G \n");
			if ((seen & LMASK('Z')) != 0) {
				for (int i = 0; i < ZstackIndex; i++) {
					if (Zstack[i].Z == Z) {
						ZstackIndex = i;
						break;
					}
				}
				//printf("Zstack: %d (%g)\n", ZstackIndex, Z);
				Zstack[ZstackIndex].start = &gcodefile[l];
				Zstack[ZstackIndex].Z = Z;
				if (ZstackIndex < 8 - 1)
					ZstackIndex++;
				else
					die("overflow while checking if Z moves are related to hop","");
			}
			if (((seen & LMASK('E')) != 0) && (ZstackIndex > 0) && (Z != lastZ)) {
				int i;
				for (i = 0; i < ZstackIndex; i++) {
					if (Zstack[i].Z == Z)
						break;
				}
				if (i < 8) {
					layer[layerCount].index = Zstack[i].start;
					layer[layerCount].height = Zstack[i].Z;
					layer[layerCount].flags = 0;
					lastZ = layer[layerCount].height;
					Zstack[0].start = layer[layerCount].index;
					Zstack[0].Z = layer[layerCount].height;
					ZstackIndex = 1;
					if (layerCount > 0)
						layer[layerCount - 1].size = layer[layerCount].index - layer[layerCount - 1].index;
					layerCount++;
					if ((layerCount + 1) * sizeof(layerData) > layerSize) {
						layer = (layerData *) realloc(layer, layerSize << 1);
						if (layer == NULL)
							die("Scan: realloc layer","");
						layerSize <<= 1;
					}
				}
				else
					die("Zstack: can't find Z value in stack!","this should never happen");
			}
		}
		l = end - gcodefile;
	}

	printf("....ZstackIndex... %ld", ZstackIndex);
	//for (int i = ZstackIndex - 1; i >= 0; i--) {
	//	printf("Zstack %d:\n\tindex %d\n\theight %g\n", i, Zstack[i].start - gcodefile, Zstack[i].Z);
	//}

	if (layerCount == 0) {
		if (ZstackIndex) {
			//printf("only one layer found!\n");
			layer[layerCount].index = gcodefile;
			layer[layerCount].height = Zstack[0].Z;
			layer[layerCount].flags = 0;
			layer[layerCount].size = filesz;
			//printf("layer starts at %d (of %d) and is %d long and is %g high\n", layer[layerCount].index - gcodefile, filesz, layer[layerCount].size, layer[layerCount].height);
			layerCount++;
		}
		else {
			die("..No layers detected in input file!","");
		}
	}

	if (layerCount > 0)
		layer[layerCount - 1].size = &gcodefile[filesz] - layer[layerCount - 1].index;
	else
		exit(0);

	printf("%d layers OK\n", layerCount);

	layer = (layerData *) realloc(layer, layerCount * sizeof(layerData));
	if (layer == NULL)
		die("Scan: realloc layer","");
	layerSize = layerCount * sizeof(layerData);
}

/***************************************************************************\
*                                                                           *
* SDL Event Handlers                                                        *
*                                                                           *
\***************************************************************************/

void handle_mousedown(SDL_MouseButtonEvent button) {
	//printf("SDL Mousebutton down event: mouse %d, button %d, state %d, %dx%d\n", Event.button.which, Event.button.button, Event.button.state, Event.button.x, Event.button.y);
	switch (button.button) {
		case 1: // left mouse
			{
				float mousex = button.x;
				float mousey = Surf_Display->h - button.y;
				float w = Surf_Display->w;
				float h = Surf_Display->h;
				float dim = minf(w, h);
				gXmouseDown = transX + (mousex / w) * 200.0 * w / dim / zoomFactor;
				gYmouseDown = transY + (mousey / h) * 200.0 * h / dim / zoomFactor;
				if (timerDragRender)
					SDL_RemoveTimer(timerDragRender);
				timerDragRender = SDL_AddTimer(50, &timerCallback, (void *) TIMER_DRAGRENDER);
			}
			break;
		case 2: // middle mouse
			break;
		case 3: // right mouse
			break;
		case 4: // wheel up
			if ((keymodifiermask & (KMM_LSHIFT | KMM_RSHIFT)) == 0) {
			#ifdef	OPENGL
				float mousex = button.x;
				float mousey = Surf_Display->h - button.y;
				float w = Surf_Display->w;
				float h = Surf_Display->h;
				float dim = minf(w, h);
				float gX = transX + (mousex / w) * 200.0 * w / dim / zoomFactor;
				float gY = transY + (mousey / h) * 200.0 * h / dim / zoomFactor;
				//printf("%d,%d->%d,%d\n", (int) transX, (int) transY, (int) gX, (int) gY);
				zoomFactor *= 1.1;
				transX = gX - (mousex / w) * 200.0 * w / dim / zoomFactor;
				transY = gY - (mousey / h) * 200.0 * h / dim/ zoomFactor;
			#else
				//float viewX = (gX - viewPortL) * zoomFactor,
				float gX = ((float) button.x) / zoomFactor + viewPortL;
				// float viewY = (viewPortB - gY) * zoomFactor,
				float gY = viewPortB - ((float) button.y) / zoomFactor;
				zoomFactor *= 1.1;
				//printf("Zoom %g\n", zoomFactor);
				viewPortL = gX - ((float) button.x) / zoomFactor;
				viewPortB = ((float) button.y) / zoomFactor + gY;
			#endif
				render();
			}
			else if (currentLayer > 0)
				drawLayer(--currentLayer);
			break;
		case 5: // wheel down
			if ((keymodifiermask & (KMM_LSHIFT | KMM_RSHIFT)) == 0) {
			#ifdef	OPENGL
				float mousex = button.x;
				float mousey = Surf_Display->h - button.y;
				float w = Surf_Display->w;
				float h = Surf_Display->h;
				float dim = minf(w, h);
				float gX = transX + (mousex / w) * 200.0 * w / dim / zoomFactor;
				float gY = transY + (mousey / h) * 200.0 * h / dim / zoomFactor;
				//printf("%d,%d->%d,%d\n", (int) transX, (int) transY, (int) gX, (int) gY);
				zoomFactor /= 1.1;
				transX = gX - (mousex / w) * 200.0 * w / dim / zoomFactor;
				transY = gY - (mousey / h) * 200.0 * h / dim / zoomFactor;
			#else
				//float viewX = (gX - viewPortL) * zoomFactor,
				float gX = ((float) button.x) / zoomFactor + viewPortL;
				// float viewY = (viewPortB - gY) * zoomFactor,
				float gY = viewPortB - ((float) button.y) / zoomFactor;
				zoomFactor /= 1.1;
				//printf("Zoom %g\n", zoomFactor);
				viewPortL = gX - ((float) button.x) / zoomFactor;
				viewPortB = ((float) button.y) / zoomFactor + gY;
			#endif
			}
			else if (currentLayer < layerCount - 1)
				drawLayer(++currentLayer);
			render();
			break;
	}
}

void handle_mousemove(SDL_MouseMotionEvent motion) {
	if (motion.state & 1) {	// left-drag
		float mousex = motion.x;
		float mousey = Surf_Display->h - motion.y;
		float w = Surf_Display->w;
		float h = Surf_Display->h;
		float dim = minf(w, h);
		transX = gXmouseDown - (mousex / w) * 200.0 * w / dim / zoomFactor;
		transY = gYmouseDown - (mousey / h) * 200.0 * h / dim / zoomFactor;
		//transZ = gXmouseDown - (mousex / w) * 200.0 * w / dim / zoomFactor;
	}
}

void handle_mouseup(SDL_MouseButtonEvent button) {
	switch (button.button) {
		case 1: // left mouse
			if (timerDragRender) {
				SDL_RemoveTimer(timerDragRender);
				timerDragRender = NULL;
			}
			break;
	}
}

void handle_keydown(SDL_KeyboardEvent key) {
	switch(key.keysym.sym) {
		case SDLK_q:
		case SDLK_ESCAPE:
			printf("Exiting\n");
			Running = false;
			break;
		case SDLK_r:
			printf("Resetting position\n");
			zoomFactor = 3;
			#ifdef	OPENGL
				transX = transY = 0.0;
			#else
				viewPortL = 0.0;
				viewPortB = 200.0;
			#endif
			resize(600, 600);
			render();
			break;
		case SDLK_PAGEUP:
			layerVelocity = 1;
			if (currentLayer < layerCount - 1)
				drawLayer(++currentLayer);
			if (timerKeyRepeat)
				SDL_RemoveTimer(timerKeyRepeat);
			timerKeyRepeat = SDL_AddTimer(500, &timerCallback, (void *) TIMER_KEYREPEAT);
			break;
		case SDLK_PAGEDOWN:
			layerVelocity = -1;
			if (currentLayer > 0)
				drawLayer(--currentLayer);
			if (timerKeyRepeat)
				SDL_RemoveTimer(timerKeyRepeat);
			timerKeyRepeat = SDL_AddTimer(500, &timerCallback, (void *) TIMER_KEYREPEAT);
			break;
		case SDLK_LSHIFT:
			keymodifiermask |= KMM_LSHIFT;
			break;
		case SDLK_RSHIFT:
			keymodifiermask |= KMM_RSHIFT;
			break;
		default:
			printf("key %d pressed (%c)\n", key.keysym.sym, key.keysym.sym);
			break;
	}
}

void handle_keyup(SDL_KeyboardEvent key) {
	switch(key.keysym.sym) {
		case SDLK_PAGEUP:
			layerVelocity = 0;
			if (timerKeyRepeat) {
				SDL_RemoveTimer(timerKeyRepeat);
				timerKeyRepeat = NULL;
			}
			break;
		case SDLK_PAGEDOWN:
			layerVelocity = 0;
			if (timerKeyRepeat) {
				SDL_RemoveTimer(timerKeyRepeat);
				timerKeyRepeat = NULL;
			}
			break;
		case SDLK_LSHIFT:
			keymodifiermask &= ~KMM_LSHIFT;
			break;
		case SDLK_RSHIFT:
			keymodifiermask &= ~KMM_RSHIFT;
			break;
		default:
			break;
	}
}

void handle_userevent(SDL_UserEvent user) {
	switch (user.code) {
		case TIMER_KEYREPEAT:
			if (layerVelocity > 0) {
				if (currentLayer < layerCount - 1)
					drawLayer(++currentLayer);
			}
			else if (layerVelocity < 0) {
				if (currentLayer > 0)
					drawLayer(--currentLayer);
			}
			break;
		case TIMER_DRAGRENDER:
			render();
			break;
	}
}

/***************************************************************************\
*                                                                           *
* Main                                                                      *
*                                                                           *
* Read GCODE, Initialise SDL window and OpenGL surface, Start FTGL, run SDL *
* Event loop                                                                *
*                                                                           *
\***************************************************************************/

int main (int argc, char *argv[]) {
	msgbuf = (char *)malloc(256);
	msgbuf[0] = 0;

	currentLayer = 0;

	int longIndex;
	int opt;
	int i;

	FILE *stream;
	int  fd, count = 0;
	char inbuf[128];

	struct __stat64 buf;
	int result;

	/*if argv[1][0] == '-' && argv[1][1] == '-'*/
	printf ( "argc = %d\n", argc );
	for ( i = 0; i < argc; i++ )
	{
		if (argv[i][0] == '-') 
		{
			/* Use the next character to decide what to do. */
			switch (argv[i][1]) 
			{
				case 'w':	extrusionWidth = atof(argv[++i]);
							break;
				case 'l':	currentLayer = atoi(argv[++i]);
							break;
				case 'h':   /* fall-through is intentional */
				case '?':	display_usage();
							break;
				/*default:	display_usage();*/

			}
		}
		printf ( "argv[%d] = %s, number of args %d\n", i, argv[i], argc );
	} // end parsing commnd line arguments

	/* Get data associated with "crt_stat.c": */
	result = _stat64( argv[argc-1], &buf );
	/* Check if statiscs are valid: */
	if( result != 0 )
	{
      perror( "Problem getting information" );
	}
	else
	{
		printf( "File size     : %ld\n", buf.st_size );
		filesz = buf.st_size;
	}
	
	HANDLE f = CreateFile(argv[argc-1], GENERIC_READ, FILE_SHARE_READ,  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); 
    HANDLE m; 
	int length;
    /*char *p; */
    if (!f) return NULL; 
    m = CreateFileMapping(f, NULL, PAGE_READONLY, 0,0, NULL); 
    if (!m) { CloseHandle(f); return NULL; } 
    gcodefile = (char *)MapViewOfFile(m, FILE_MAP_READ, 0,0,0); 
    if (!gcodefile) { CloseHandle(m); CloseHandle(f); return NULL; } 
    length = GetFileSize(f, NULL); 
     
	
	printf( "length File size     : %ld\n", length );
	printf( "length File size     : %c\n", gcodefile[0] );

	
	scanLines();

	if (currentLayer >= layerCount)
		currentLayer = layerCount - 1;

	Running = true;
	Surf_Display = NULL;

	if (SDL_Init(SDL_INIT_EVERYTHING) < 0)
		perror( "Problem SDL_init" );//die("SDL_init", "");

	printf( "Past SDL_Init\n");
	#ifdef	OPENGL
		transX = transY = transZ = 0.0;
		zoomFactor = 1.0;

		resize(600, 600);

		//printf("list base at %d, %d lists available\n", glListsBase, layerCount);
	#else
		viewPortL = viewPortT = 0.0;
		viewPortR = viewPortB = 200.0;
		zoomFactor = 3.0;
		resize(viewPortR * zoomFactor, viewPortB * zoomFactor);*/
	#endif

	SDL_WM_SetCaption("gcodeview", 0);

	drawLayer(currentLayer);

	layerVelocity = 0;
	
	//printf( "Past SDL_WM_SetCaption\n");
	SDL_Event Event;
	while(Running != false) 
	{
		if (SDL_WaitEvent(&Event) == 0)
		{
			perror( "Problem SDL_WaitEvent" );
		}
		switch (Event.type) 
		{
			case SDL_QUIT:
				Running = false;
				break;
			case SDL_VIDEORESIZE:
				resize(Event.resize.w, Event.resize.h);
				break;
			case SDL_VIDEOEXPOSE:
				render();
				break;
			case SDL_MOUSEBUTTONDOWN:
				handle_mousedown(Event.button);
				break;
			case SDL_MOUSEBUTTONUP:
				handle_mouseup(Event.button);
				break;
			case SDL_MOUSEMOTION:
				handle_mousemove(Event.motion);
				break;
			case SDL_ACTIVEEVENT: // lose or gain focus
				break;
			case SDL_KEYDOWN:
				handle_keydown(Event.key);
				break;
			case SDL_KEYUP:
				handle_keyup(Event.key);
				break;
			case SDL_USEREVENT:
				handle_userevent(Event.user);
				break;
			default:
				printf("SDL Event %d\n", Event.type);
				break;
		}

	}

	UnmapViewOfFile(gcodefile);
 //   CloseHandle(hMapFile);
  //  CloseHandle(hFile);
    return 0;
}