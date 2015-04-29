/*
 cm.grainlabs~ - a granular synthesis external audio object for Max/MSP.
 Copyright (C) 2014  Matthias Müller - Circuit Music Labs
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 
 circuit.music.labs@gmail.com
 
 */

/************************************************************************************************************************/
/* INCLUDES                                                                                                             */
/************************************************************************************************************************/
#include "ext.h"
#include "z_dsp.h"
#include "buffer.h"
#include "ext_atomic.h"
#include "ext_obex.h"
#include "cmstereo.h" // for cm_pan
#include "cmutil.h" // for cm_random and cm_grainsinfo (struct)
#include <stdlib.h> // for arc4random_uniform
#define MAX_GRAINLENGTH 300 // max grain length in ms
#define MIN_GRAINLENGTH 1 // min grain length in ms
#define MAX_PITCH 10 // max pitch
#define ARGUMENTS 3 // constant number of arguments required for the external
#define MAXGRAINS 128 // maximum number of simultaneously playing grains


/************************************************************************************************************************/
/* GRAIN INFORMATION STRUCTURE                                                                                          */
/************************************************************************************************************************/
typedef struct cmgrainsinfo {
	short busy;
	long grainpos;
	long start;
	long t_length;
	long gr_length;
	double pan_left;
	double pan_right;
} cm_grainsinfo;


/************************************************************************************************************************/
/* OBJECT STRUCTURE                                                                                                     */
/************************************************************************************************************************/
typedef struct _cmgrainlabs {
	t_pxobject obj;
	t_symbol *buffer_name; // sample buffer name
	t_buffer_ref *buffer; // sample buffer reference
	t_symbol *window_name; // window buffer name
	t_buffer_ref *w_buffer; // window buffer reference
	double m_sr; // system millisampling rate (samples per milliseconds = sr * 0.001)
	double startmin_float; // grain start min value received from float inlet
	double startmax_float; // grain start max value received from float inlet
	double lengthmin_float; // used to store the min length value received from float inlet
	double lengthmax_float; // used to store the max length value received from float inlet
	double pitchmin_float; // used to store the min pitch value received from float inlet
	double pitchmax_float; // used to store the max pitch value received from float inlet
	double panmin_float; // used to store the min pan value received from the float inlet
	double panmax_float; // used to store the max pan value received from the float inlet
	short connect_status[8]; // array for signal inlet connection statuses
	cm_grainsinfo *grains; // pointer to struct for grain storing grain playback information
	double tr_prev; // trigger sample from previous signal vector (required to check if input ramp resets to zero)
	short grains_limit; // user defined maximum number of grains
	short grains_limit_old; // used to store the previous grains count limit when user changes the limit via the "limit" message
	short limit_modified; // checkflag to see if user changed grain limit through "limit" method
	short buffer_modified; // checkflag to see if buffer has been modified
	short grains_count; // currently playing grains
	void *grains_count_out; // outlet for number of currently playing grains (for debugging)
	t_atom_long attr_stereo; // attribute: number of channels to be played
	t_atom_long attr_winterp; // attribute: window interpolation on/off
	t_atom_long attr_sinterp; // attribute: window interpolation on/off
	t_atom_long attr_zero; // attribute: zero crossing trigger on/off
} t_cmgrainlabs;


/************************************************************************************************************************/
/* STATIC DECLARATIONS                                                                                                  */
/************************************************************************************************************************/
static t_class *cmgrainlabs_class; // class pointer
static t_symbol *ps_buffer_modified, *ps_stereo;


/************************************************************************************************************************/
/* FUNCTION PROTOTYPES                                                                                                  */
/************************************************************************************************************************/
void *cmgrainlabs_new(t_symbol *s, long argc, t_atom *argv);
void cmgrainlabs_dsp64(t_cmgrainlabs *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void cmgrainlabs_perform64(t_cmgrainlabs *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void cmgrainlabs_assist(t_cmgrainlabs *x, void *b, long msg, long arg, char *dst);
void cmgrainlabs_free(t_cmgrainlabs *x);
void cmgrainlabs_float(t_cmgrainlabs *x, double f);
void cmgrainlabs_dblclick(t_cmgrainlabs *x);
t_max_err cmgrainlabs_notify(t_cmgrainlabs *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void cmgrainlabs_set(t_cmgrainlabs *x, t_symbol *s, long ac, t_atom *av);
void cmgrainlabs_limit(t_cmgrainlabs *x, t_symbol *s, long ac, t_atom *av);
t_max_err cmgrainlabs_stereo_set(t_cmgrainlabs *x, t_object *attr, long argc, t_atom *argv);
t_max_err cmgrainlabs_winterp_set(t_cmgrainlabs *x, t_object *attr, long argc, t_atom *argv);
t_max_err cmgrainlabs_sinterp_set(t_cmgrainlabs *x, t_object *attr, long argc, t_atom *argv);
t_max_err cmgrainlabs_zero_set(t_cmgrainlabs *x, t_object *attr, long argc, t_atom *argv);


/************************************************************************************************************************/
/* MAIN FUNCTION (INITIALIZATION ROUTINE)                                                                               */
/************************************************************************************************************************/
int C74_EXPORT main(void) {
	// Initialize the class - first argument: VERY important to match the name of the object in the procect settings!!!
	cmgrainlabs_class = class_new("cm.grainlabs~", (method)cmgrainlabs_new, (method)cmgrainlabs_free, sizeof(t_cmgrainlabs), 0, A_GIMME, 0);
	
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_dsp64, 		"dsp64", 	A_CANT, 0);  // Bind the 64 bit dsp method
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_assist, 		"assist", 	A_CANT, 0); // Bind the assist message
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_float, 		"float", 	A_FLOAT, 0); // Bind the float message (allowing float input)
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_dblclick, 	"dblclick",	A_CANT, 0); // Bind the double click message
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_notify, 		"notify", 	A_CANT, 0); // Bind the notify message
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_set, 		"set", 		A_GIMME, 0); // Bind the set message for user buffer set
	class_addmethod(cmgrainlabs_class, (method)cmgrainlabs_limit, 		"limit", 	A_GIMME, 0); // Bind the limit message
	
	CLASS_ATTR_ATOM_LONG(cmgrainlabs_class, "stereo", 0, t_cmgrainlabs, attr_stereo);
	CLASS_ATTR_ACCESSORS(cmgrainlabs_class, "stereo", (method)NULL, (method)cmgrainlabs_stereo_set);
	CLASS_ATTR_BASIC(cmgrainlabs_class, "stereo", 0);
	CLASS_ATTR_SAVE(cmgrainlabs_class, "stereo", 0);
	CLASS_ATTR_STYLE_LABEL(cmgrainlabs_class, "stereo", 0, "onoff", "Multichannel playback");
	
	CLASS_ATTR_ATOM_LONG(cmgrainlabs_class, "w_interp", 0, t_cmgrainlabs, attr_winterp);
	CLASS_ATTR_ACCESSORS(cmgrainlabs_class, "w_interp", (method)NULL, (method)cmgrainlabs_winterp_set);
	CLASS_ATTR_BASIC(cmgrainlabs_class, "w_interp", 0);
	CLASS_ATTR_SAVE(cmgrainlabs_class, "w_interp", 0);
	CLASS_ATTR_STYLE_LABEL(cmgrainlabs_class, "w_interp", 0, "onoff", "Window interpolation on/off");
	
	CLASS_ATTR_ATOM_LONG(cmgrainlabs_class, "s_interp", 0, t_cmgrainlabs, attr_sinterp);
	CLASS_ATTR_ACCESSORS(cmgrainlabs_class, "s_interp", (method)NULL, (method)cmgrainlabs_sinterp_set);
	CLASS_ATTR_BASIC(cmgrainlabs_class, "s_interp", 0);
	CLASS_ATTR_SAVE(cmgrainlabs_class, "s_interp", 0);
	CLASS_ATTR_STYLE_LABEL(cmgrainlabs_class, "s_interp", 0, "onoff", "Sample interpolation on/off");
	
	CLASS_ATTR_ATOM_LONG(cmgrainlabs_class, "zero", 0, t_cmgrainlabs, attr_zero);
	CLASS_ATTR_ACCESSORS(cmgrainlabs_class, "zero", (method)NULL, (method)cmgrainlabs_zero_set);
	CLASS_ATTR_BASIC(cmgrainlabs_class, "zero", 0);
	CLASS_ATTR_SAVE(cmgrainlabs_class, "zero", 0);
	CLASS_ATTR_STYLE_LABEL(cmgrainlabs_class, "zero", 0, "onoff", "Zero crossing trigger mode on/off");
	
	CLASS_ATTR_ORDER(cmgrainlabs_class, "stereo", 0, "1");
	CLASS_ATTR_ORDER(cmgrainlabs_class, "w_interp", 0, "2");
	CLASS_ATTR_ORDER(cmgrainlabs_class, "s_interp", 0, "3");
	
	class_dspinit(cmgrainlabs_class); // Add standard Max/MSP methods to your class
	class_register(CLASS_BOX, cmgrainlabs_class); // Register the class with Max
	ps_buffer_modified = gensym("buffer_modified"); // assign the buffer modified message to the static pointer created above
	ps_stereo = gensym("stereo");
	return 0;
}


/************************************************************************************************************************/
/* NEW INSTANCE ROUTINE                                                                                                 */
/************************************************************************************************************************/
void *cmgrainlabs_new(t_symbol *s, long argc, t_atom *argv) {
	int i; // for loop counter
	t_cmgrainlabs *x = (t_cmgrainlabs *)object_alloc(cmgrainlabs_class); // create the object and allocate required memory
	dsp_setup((t_pxobject *)x, 9); // create 9 inlets
	
	if (argc < ARGUMENTS) {
		object_error((t_object *)x, "%d arguments required (sample/window/voices)", ARGUMENTS);
		return NULL;
	}
	
	x->buffer_name = atom_getsymarg(0, argc, argv); // get user supplied argument for sample buffer
	x->window_name = atom_getsymarg(1, argc, argv); // get user supplied argument for window buffer
	x->grains_limit = atom_getintarg(2, argc, argv); // get user supplied argument for maximum grains
	
	// HANDLE ATTRIBUTES
	object_attr_setlong(x, gensym("stereo"), 0); // initialize stereo attribute
	object_attr_setlong(x, gensym("w_interp"), 0); // initialize window interpolation attribute
	object_attr_setlong(x, gensym("s_interp"), 1); // initialize window interpolation attribute
	object_attr_setlong(x, gensym("zero"), 0); // initialize zero crossing attribute
	attr_args_process(x, argc, argv); // get attribute values if supplied as argument
	
	// CHECK IF USER SUPPLIED MAXIMUM GRAINS IS IN THE LEGAL RANGE (1 - MAXGRAINS)
	if (x->grains_limit < 1 || x->grains_limit > MAXGRAINS) {
		object_error((t_object *)x, "maximum grains allowed is %d", MAXGRAINS);
		return NULL;
	}
	
	// CREATE OUTLETS (OUTLETS ARE CREATED FROM RIGHT TO LEFT)
	x->grains_count_out = intout((t_object *)x); // create outlet for number of currently playing grains
	outlet_new((t_object *)x, "signal"); // right signal outlet
	outlet_new((t_object *)x, "signal"); // left signal outlet
	
	// GET SYSTEM SAMPLE RATE
	x->m_sr = sys_getsr() * 0.001; // get the current sample rate and write it into the object structure
	
	/************************************************************************************************************************/
	// ALLOCATE MEMORY FOR THE GRAINSINFO ARRAY
	x->grains = (cm_grainsinfo *)sysmem_newptr((MAXGRAINS) * sizeof(cm_grainsinfo *));
	if (x->grains == NULL) {
		object_error((t_object *)x, "out of memory");
		return NULL;
	}
	
	// INITIALIZE VALUES FOR THE GRAINSINFO ARRAY
	for (i = 0; i < MAXGRAINS; i++) {
		x->grains[i].busy = 0;
		x->grains[i].grainpos = 0;
		x->grains[i].start = 0;
		x->grains[i].t_length = 0;
		x->grains[i].gr_length = 0;
		x->grains[i].pan_left = 0.0;
		x->grains[i].pan_right = 0.0;
	}
	
	/************************************************************************************************************************/
	// INITIALIZE VALUES
	x->startmin_float = 0.0; // initialize float inlet value for current start min value
	x->startmax_float = 0.0; // initialize float inlet value for current start max value
	x->lengthmin_float = 150; // initialize float inlet value for min grain length
	x->lengthmax_float = 150; // initialize float inlet value for max grain length
	x->pitchmin_float = 1.0; // initialize inlet value for min pitch
	x->pitchmax_float = 1.0; // initialize inlet value for min pitch
	x->panmin_float = 0.0; // initialize value for min pan
	x->panmax_float = 0.0; // initialize value for max pan
	x->tr_prev = 0.0; // initialize value for previous trigger sample
	x->grains_count = 0; // initialize the grains count value
	x->grains_limit_old = 0; // initialize value for the routine when grains limit was modified
	x->limit_modified = 0; // initialize channel change flag
	x->buffer_modified = 0; // initialized buffer modified flag
	
	/************************************************************************************************************************/
	// BUFFER REFERENCES
	x->buffer = buffer_ref_new((t_object *)x, x->buffer_name); // write the buffer reference into the object structure
	x->w_buffer = buffer_ref_new((t_object *)x, x->window_name); // write the window buffer reference into the object structure
	
	return x;
}


/************************************************************************************************************************/
/* THE 64 BIT DSP METHOD                                                                                                */
/************************************************************************************************************************/
void cmgrainlabs_dsp64(t_cmgrainlabs *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
	x->connect_status[0] = count[1]; // 2nd inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[1] = count[2]; // 3rd inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[2] = count[3]; // 4th inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[3] = count[4]; // 5th inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[4] = count[5]; // 6th inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[5] = count[6]; // 7th inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[6] = count[7]; // 8th inlet: write connection flag into object structure (1 if signal connected)
	x->connect_status[7] = count[8]; // 9th inlet: write connection flag into object structure (1 if signal connected)
	
	if (x->m_sr != samplerate * 0.001) { // check if sample rate stored in object structure is the same as the current project sample rate
		x->m_sr = samplerate * 0.001;
	}
	
	// CALL THE PERFORM ROUTINE
	//object_method(dsp64, gensym("dsp_add64"), x, cmgrainlabs_perform64, 0, NULL);
	dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)cmgrainlabs_perform64, 0, NULL);
}


/************************************************************************************************************************/
/* THE 64 BIT PERFORM ROUTINE                                                                                           */
/************************************************************************************************************************/
void cmgrainlabs_perform64(t_cmgrainlabs *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
	// VARIABLE DECLARATIONS
	short trigger = 0; // trigger occurred yes/no
	long i, limit; // for loop counterS
	long n = sampleframes; // number of samples per signal vector
	double tr_curr; // current trigger value
	double pan; // temporary random pan information
	double pitch; // temporary pitch for new grains
	double distance; // floating point index for reading from buffers
	long index; // truncated index for reading from buffers
	double w_read, b_read; // current sample read from the window buffer
	double outsample_left = 0.0; // temporary left output sample used for adding up all grain samples
	double outsample_right = 0.0; // temporary right output sample used for adding up all grain samples
	int slot = 0; // variable for the current slot in the arrays to write grain info to
	cm_panstruct pan_in; // temporary pan information structure (maybe not necessary)
	
	// OUTLETS
	t_double *out_left 	= (t_double *)outs[0]; // assign pointer to left output
	t_double *out_right = (t_double *)outs[1]; // assign pointer to right output
	
	// BUFFER VARIABLE DECLARATIONS
	t_buffer_obj *buffer = buffer_ref_getobject(x->buffer);
	t_buffer_obj *w_buffer = buffer_ref_getobject(x->w_buffer);
	float *b_sample = buffer_locksamples(buffer);
	float *w_sample = buffer_locksamples(w_buffer);
	long b_framecount; // number of frames in the sample buffer
	long w_framecount; // number of frames in the window buffer
	t_atom_long b_channelcount; // number of channels in the sample buffer
	t_atom_long w_channelcount; // number of channels in the window buffer
	
	// BUFFER CHECKS
	if (!b_sample) { // if the sample buffer does not exist
		goto zero;
	}
	if (!w_sample) { // if the window buffer does not exist
		goto zero;
	}
	
	// GET BUFFER INFORMATION
	b_framecount = buffer_getframecount(buffer); // get number of frames in the sample buffer
	w_framecount = buffer_getframecount(w_buffer); // get number of frames in the window buffer
	b_channelcount = buffer_getchannelcount(buffer); // get number of channels in the sample buffer
	w_channelcount = buffer_getchannelcount(w_buffer); // get number of channels in the sample buffer
	
	// GET INLET VALUES
	t_double *tr_sigin 	= (t_double *)ins[0]; // get trigger input signal from 1st inlet
	t_double startmin 	= x->connect_status[0]? *ins[1] * x->m_sr : x->startmin_float * x->m_sr; // get start min input signal from 2nd inlet
	t_double startmax 	= x->connect_status[1]? *ins[2] * x->m_sr : x->startmax_float * x->m_sr; // get start max input signal from 3rd inlet
	t_double lengthmin 	= x->connect_status[2]? *ins[3] * x->m_sr : x->lengthmin_float * x->m_sr; // get grain min length input signal from 4th inlet
	t_double lengthmax 	= x->connect_status[3]? *ins[4] * x->m_sr : x->lengthmax_float * x->m_sr; // get grain max length input signal from 5th inlet
	t_double pitchmin 	= x->connect_status[4]? *ins[5] : x->pitchmin_float; // get pitch min input signal from 6th inlet
	t_double pitchmax 	= x->connect_status[5]? *ins[6] : x->pitchmax_float; // get pitch max input signal from 7th inlet
	t_double panmin 	= x->connect_status[6]? *ins[7] : x->panmin_float; // get min pan input signal from 8th inlet
	t_double panmax 	= x->connect_status[7]? *ins[8] : x->panmax_float; // get max pan input signal from 8th inlet
	
	// DSP LOOP
	while (n--) {
		tr_curr = *tr_sigin++; // get current trigger value
		
		if (x->attr_zero) {
			if (tr_curr > 0.0 && x->tr_prev < 0.0) { // zero crossing from negative to positive
				trigger = 1;
			}
		}
		else {
			if ((x->tr_prev - tr_curr) > 0.9) {
				trigger = 1;
			}
		}
		
		if (x->buffer_modified) { // reset all playback information when any of the buffers was modified
			for (i = 0; i < MAXGRAINS; i++) {
				x->grains[i].busy = 0;
			}
			x->grains_count = 0;
			x->buffer_modified = 0;
		}
		/************************************************************************************************************************/
		// IN CASE OF TRIGGER, LIMIT NOT MODIFIED AND GRAINS COUNT IN THE LEGAL RANGE (AVAILABLE SLOTS)
		if (trigger && x->grains_count < x->grains_limit && !x->limit_modified) { // based on zero crossing --> when ramp from 0-1 restarts.
			trigger = 0; // reset trigger
			x->grains_count++; // increment grains_count
			// FIND A FREE SLOT FOR THE NEW GRAIN
			i = 0;
			while (i < x->grains_limit) {
				if (!x->grains[i].busy) {
					x->grains[i].busy = 1;
					slot = i;
					break;
				}
				i++;
			}
			/************************************************************************************************************************/
			// GET RANDOM START POSITION
			if (startmin != startmax) { // only call random function when min and max values are not the same!
				x->grains[slot].start = (long)cm_random(&startmin, &startmax);
			}
			else {
				x->grains[slot].start = startmin;
			}
			/************************************************************************************************************************/
			// GET RANDOM LENGTH
			if (lengthmin != lengthmax) { // only call random function when min and max values are not the same!
				x->grains[slot].t_length = (long)cm_random(&lengthmin, &lengthmax);
			}
			else {
				x->grains[slot].t_length = lengthmin;
			}
			// CHECK IF THE VALUE FOR PERCEPTIBLE GRAIN LENGTH IS LEGAL
			if (x->grains[slot].t_length > MAX_GRAINLENGTH * x->m_sr) { // if grain length is larger than the max grain length
				x->grains[slot].t_length = MAX_GRAINLENGTH * x->m_sr; // set grain length to max grain length
			}
			else if (x->grains[slot].t_length < MIN_GRAINLENGTH * x->m_sr) { // if grain length is samller than the min grain length
				x->grains[slot].t_length = MIN_GRAINLENGTH * x->m_sr; // set grain length to min grain length
			}
			/************************************************************************************************************************/
			// GET RANDOM PAN
			if (panmin != panmax) { // only call random function when min and max values are not the same!
				pan = cm_random(&panmin, &panmax);
			}
			else {
				pan = panmin;
			}
			// SOME SANITY TESTING
			if (pan < -1.0) {
				pan = -1.0;
			}
			if (pan > 1.0) {
				pan = 1.0;
			}
			// CALCULATE PANNING INFORMATION
			cm_panning(&pan_in, &pan); // calculate pan values inside the panstructure
			
			// WRITE PAN VALUES INTO GRAINSINFO STRUCTURE
			x->grains[slot].pan_left = pan_in.left;
			x->grains[slot].pan_right = pan_in.right;
			
			/************************************************************************************************************************/
			// GET RANDOM PITCH
			if (pitchmin != pitchmax) { // only call random function when min and max values are not the same!
				pitch = cm_random(&pitchmin, &pitchmax);
			}
			else {
				pitch = pitchmin;
			}
			// CHECK IF THE PITCH VALUE IS LEGAL
			if (pitch < 0.001) {
				pitch = 0.001;
			}
			if (pitch > MAX_PITCH) {
				pitch = MAX_PITCH;
			}
			/************************************************************************************************************************/
			// CALCULATE THE ACTUAL GRAIN LENGTH (SAMPLES) ACCORDING TO PITCH
			x->grains[slot].gr_length = x->grains[slot].t_length * pitch;
			// CHECK THAT GRAIN LENGTH IS NOT LARGER THAN SIZE OF BUFFER
			if (x->grains[slot].gr_length > b_framecount) {
				x->grains[slot].gr_length = b_framecount;
			}
			/************************************************************************************************************************/
			// CHECK IF START POSITION IS LEGAL ACCORDING TO GRAINzLENGTH (SAMPLES) AND BUFFER SIZE
			if (x->grains[slot].start > b_framecount - x->grains[slot].gr_length) {
				x->grains[slot].start = b_framecount - x->grains[slot].gr_length;
			}
			if (x->grains[slot].start < 0) {
				x->grains[slot].start = 0;
			}
		}
		/************************************************************************************************************************/
		// CONTINUE WITH THE PLAYBACK ROUTINE
		if (x->grains_count == 0) { // if grains count is zero, there is no playback to be calculated
			*out_left++ = 0.0;
			*out_right++ = 0.0;
		}
		else if (!b_sample) {
			*out_left++ = 0.0;
			*out_right++ = 0.0;
		}
		else if (!w_sample) {
			*out_left++ = 0.0;
			*out_right++ = 0.0;
		}
		else {
			if (x->limit_modified) {
				limit = x->grains_limit_old;
			}
			else {
				limit = x->grains_limit;
			}
			for (i = 0; i < limit; i++) {
				if (x->grains[i].busy) { // if the current slot contains grain playback information
					// GET WINDOW SAMPLE FROM WINDOW BUFFER
					if (x->attr_winterp) {
						distance = ((double)x->grains[i].grainpos / (double)x->grains[i].t_length) * (double)w_framecount;
						w_read = cm_lininterp(distance, w_sample, w_channelcount, 0);
					}
					else {
						index = (long)(((double)x->grains[i].grainpos / (double)x->grains[i].t_length) * (double)w_framecount);
						w_read = w_sample[index];
					}
					// GET GRAIN SAMPLE FROM SAMPLE BUFFER
					distance = x->grains[i].start + (((double)x->grains[i].grainpos++ / (double)x->grains[i].t_length) * (double)x->grains[i].gr_length);
					
					if (b_channelcount > 1 && x->attr_stereo) { // if more than one channel
						if (x->attr_sinterp) {
							outsample_left += (cm_lininterp(distance, b_sample, b_channelcount, 0) * w_read) * x->grains[i].pan_left; // get interpolated sample
							outsample_right += (cm_lininterp(distance, b_sample, b_channelcount, 1) * w_read) * x->grains[i].pan_right;
						}
						else {
							outsample_left += (b_sample[(long)distance * b_channelcount] * w_read) * x->grains[i].pan_left;
							outsample_right += (b_sample[((long)distance * b_channelcount) + 1] * w_read) * x->grains[i].pan_right;
						}
					}
					else {
						if (x->attr_sinterp) {
							b_read = cm_lininterp(distance, b_sample, b_channelcount, 0) * w_read; // get interpolated sample
							outsample_left += b_read * x->grains[i].pan_left;
							outsample_right += b_read * x->grains[i].pan_right;
						}
						else {
							outsample_left += (b_sample[(long)distance * b_channelcount] * w_read) * x->grains[i].pan_left;
							outsample_right += (b_sample[(long)distance * b_channelcount] * w_read) * x->grains[i].pan_right;
						}
					}
					if (x->grains[i].grainpos == x->grains[i].t_length) { // if current grain has reached the end position
						x->grains[i].grainpos = 0; // reset parameters for overwrite
						x->grains[i].busy = 0;
						x->grains_count--;
						if (x->grains_count < 0) {
							x->grains_count = 0;
						}
					}
				}
			}
			*out_left++ = outsample_left; // write added sample values to left output vector
			*out_right++ = outsample_right; // write added sample values to right output vector
		}
		// CHECK IF GRAINS COUNT IS ZERO, THEN RESET LIMIT_MODIFIED CHECKFLAG
		if (x->grains_count == 0) {
			x->limit_modified = 0; // reset limit modified checkflag
		}
		
		/************************************************************************************************************************/
		x->tr_prev = tr_curr; // store current trigger value in object structure
		outsample_left = 0.0;
		outsample_right = 0.0;
	}
	
	/************************************************************************************************************************/
	// STORE UPDATED RUNNING VALUES INTO THE OBJECT STRUCTURE
	buffer_unlocksamples(buffer);
	buffer_unlocksamples(w_buffer);
	outlet_int(x->grains_count_out, x->grains_count); // send number of currently playing grains to the outlet
	return;
	
zero:
	while (n--) {
		*out_left++ = 0.0;
		*out_right++ = 0.0;
	}
	buffer_unlocksamples(buffer);
	buffer_unlocksamples(w_buffer);
}


/************************************************************************************************************************/
/* ASSIST METHOD FOR INLET AND OUTLET ANNOTATION                                                                        */
/************************************************************************************************************************/
void cmgrainlabs_assist(t_cmgrainlabs *x, void *b, long msg, long arg, char *dst) {
	if (msg == ASSIST_INLET) {
		switch (arg) {
			case 0:
				snprintf_zero(dst, 256, "(signal) trigger in");
				break;
			case 1:
				snprintf_zero(dst, 256, "(signal/float) start min");
				break;
			case 2:
				snprintf_zero(dst, 256, "(signal/float) start max");
				break;
			case 3:
				snprintf_zero(dst, 256, "(signal/float) min grain length");
				break;
			case 4:
				snprintf_zero(dst, 256, "(signal/float) max grain length");
				break;
			case 5:
				snprintf_zero(dst, 256, "(signal/float) pitch min");
				break;
			case 6:
				snprintf_zero(dst, 256, "(signal/float) pitch max");
				break;
			case 7:
				snprintf_zero(dst, 256, "(signal/float) pan min");
				break;
			case 8:
				snprintf_zero(dst, 256, "(signal/float) pan max");
				break;
		}
	}
	else if (msg == ASSIST_OUTLET) {
		switch (arg) {
			case 0:
				snprintf_zero(dst, 256, "(signal) output ch1");
				break;
			case 1:
				snprintf_zero(dst, 256, "(signal) output ch2");
				break;
			case 2:
				snprintf_zero(dst, 256, "(int) current grain count");
				break;
		}
	}
}


/************************************************************************************************************************/
/* FREE FUNCTION                                                                                                        */
/************************************************************************************************************************/
void cmgrainlabs_free(t_cmgrainlabs *x) {
	dsp_free((t_pxobject *)x); // free memory allocated for the object
	object_free(x->buffer); // free the buffer reference
	object_free(x->w_buffer); // free the window buffer reference
	sysmem_freeptr(x->grains); // free memory allocated to the grains info array
}

/************************************************************************************************************************/
/* FLOAT METHOD FOR FLOAT INLET SUPPORT                                                                                 */
/************************************************************************************************************************/
void cmgrainlabs_float(t_cmgrainlabs *x, double f) {
	double dump;
	int inlet = ((t_pxobject*)x)->z_in; // get info as to which inlet was addressed (stored in the z_in component of the object structure
	switch (inlet) {
		case 1: // first inlet
			if (f < 0.0) {
				dump = f;
			}
			else {
				x->startmin_float = f;
			}
			break;
		case 2: // second inlet
			if (f < 0.0) {
				dump = f;
			}
			else {
				x->startmax_float = f;
			}
			break;
		case 3: // 4th inlet
			if (f < MIN_GRAINLENGTH) {
				dump = f;
			}
			else if (f > MAX_GRAINLENGTH) {
				dump = f;
			}
			else {
				x->lengthmin_float = f;
			}
			break;
		case 4: // 5th inlet
			if (f < MIN_GRAINLENGTH) {
				dump = f;
			}
			else if (f > MAX_GRAINLENGTH) {
				dump = f;
			}
			else {
				x->lengthmax_float = f;
			}
			break;
		case 5: // 6th inlet
			if (f <= 0.0) {
				dump = f;
			}
			else if (f > MAX_PITCH) {
				dump = f;
			}
			else {
				x->pitchmin_float = f;
			}
			break;
		case 6: // 7th inlet
			if (f <= 0.0) {
				dump = f;
			}
			else if (f > MAX_PITCH) {
				dump = f;
			}
			else {
				x->pitchmax_float = f;
			}
			break;
		case 7:
			if (f < -1.0 || f > 1.0) {
				dump = f;
			}
			else {
				x->panmin_float = f;
			}
			break;
		case 8:
			if (f < -1.0 || f > 1.0) {
				dump = f;
			}
			else {
				x->panmax_float = f;
			}
			break;
	}
}


/************************************************************************************************************************/
/* DOUBLE CLICK METHOD FOR VIEWING BUFFER CONTENT                                                                       */
/************************************************************************************************************************/
void cmgrainlabs_dblclick(t_cmgrainlabs *x) {
	buffer_view(buffer_ref_getobject(x->buffer));
	buffer_view(buffer_ref_getobject(x->w_buffer));
}


/************************************************************************************************************************/
/* NOTIFY METHOD FOR THE BUFFER REFERENCEs                                                                              */
/************************************************************************************************************************/
t_max_err cmgrainlabs_notify(t_cmgrainlabs *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
	t_symbol *buffer_name = (t_symbol *)object_method((t_object *)sender, gensym("getname"));
	if (msg == ps_buffer_modified) {
		x->buffer_modified = 1;
	}
	if (buffer_name == x->window_name) { // check if calling object was the sample buffer
		return buffer_ref_notify(x->w_buffer, s, msg, sender, data); // return with the calling buffer
	}
	else { // check if calling object was the window buffer
		return buffer_ref_notify(x->buffer, s, msg, sender, data); // return with the calling buffer
	}
}


/************************************************************************************************************************/
/* THE BUFFER SET METHOD                                                                                                */
/************************************************************************************************************************/
void cmgrainlabs_set(t_cmgrainlabs *x, t_symbol *s, long ac, t_atom *av) {
	if (ac == 2) {
		x->buffer_modified = 1;
		x->buffer_name = atom_getsym(av); // write buffer name into object structure
		x->window_name = atom_getsym(av+1); // write buffer name into object structure
		buffer_ref_set(x->buffer, x->buffer_name);
		buffer_ref_set(x->w_buffer, x->window_name);
		if (buffer_getchannelcount((t_object *)(buffer_ref_getobject(x->buffer))) > 2) {
			object_error((t_object *)x, "referenced sample buffer has more than 2 channels. using channels 1 and 2.");
		}
		if (buffer_getchannelcount((t_object *)(buffer_ref_getobject(x->w_buffer))) > 1) {
			object_error((t_object *)x, "referenced window buffer has more than 1 channel. expect strange results.");
		}
	}
	else {
		object_error((t_object *)x, "%d arguments required (sample/window)", 2);
	}
}


/************************************************************************************************************************/
/* THE GRAINS LIMIT METHOD                                                                                              */
/************************************************************************************************************************/
void cmgrainlabs_limit(t_cmgrainlabs *x, t_symbol *s, long ac, t_atom *av) {
	long arg;
	arg = atom_getlong(av);
	if (arg < 1 || arg > MAXGRAINS) {
		object_error((t_object *)x, "value must be in the range 1 - %d", MAXGRAINS);
	}
	else {
		x->grains_limit_old = x->grains_limit;
		x->grains_limit = arg;
		x->limit_modified = 1;
	}
}


/************************************************************************************************************************/
/* THE STEREO ATTRIBUTE SET METHOD                                                                                      */
/************************************************************************************************************************/
t_max_err cmgrainlabs_stereo_set(t_cmgrainlabs *x, t_object *attr, long ac, t_atom *av) {
	if (ac && av) {
		x->attr_stereo = atom_getlong(av)? 1 : 0;
	}
	return MAX_ERR_NONE;
}


/************************************************************************************************************************/
/* THE WINDOW INTERPOLATION ATTRIBUTE SET METHOD                                                                        */
/************************************************************************************************************************/
t_max_err cmgrainlabs_winterp_set(t_cmgrainlabs *x, t_object *attr, long ac, t_atom *av) {
	if (ac && av) {
		x->attr_winterp = atom_getlong(av)? 1 : 0;
	}
	return MAX_ERR_NONE;
}


/************************************************************************************************************************/
/* THE SAMPLE INTERPOLATION ATTRIBUTE SET METHOD                                                                        */
/************************************************************************************************************************/
t_max_err cmgrainlabs_sinterp_set(t_cmgrainlabs *x, t_object *attr, long ac, t_atom *av) {
	if (ac && av) {
		x->attr_sinterp = atom_getlong(av)? 1 : 0;
	}
	return MAX_ERR_NONE;
}


/************************************************************************************************************************/
/* THE ZERO CROSSING ATTRIBUTE SET METHOD                                                                               */
/************************************************************************************************************************/
t_max_err cmgrainlabs_zero_set(t_cmgrainlabs *x, t_object *attr, long ac, t_atom *av) {
	if (ac && av) {
		x->attr_zero = atom_getlong(av)? 1 : 0;
	}
	return MAX_ERR_NONE;
}

