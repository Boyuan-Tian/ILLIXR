#include "GL/gl.h"

namespace ILLIXR {
	
	struct cam_frame { };

	/* I use "accel" instead of "3-vector" as a datatype, because
	this checks that you meant to use an acceleration in a certain
	place. */
	struct accel { };

	struct pose {
		int data[6];
	};

	struct rendered_frame {
		GLuint texture_handle;
	};

	/* All methods compiled-in separately have to be virtual. */

	class abstract_cam {
	public:
		virtual cam_frame& produce_blocking() = 0;
		virtual ~abstract_cam() { }
	};

	class abstract_imu {
	public:
		virtual accel& produce_nonbl() = 0;
		virtual ~abstract_imu() { }
	};

	class abstract_timewarp {
	public:
		virtual void init(rendered_frame frame_handle) = 0;
		virtual void warp(float time);
		virtual ~abstract_timewarp() { }
	};

		/* In this implementation, all of the asynchrony happens inside
		the components. feed_cam_frame_nobl could add a camera frame to
		a queue. produce_nobl can be read from a double buffer. */
	class abstract_slam {
	public:
		virtual void feed_cam_frame_nonbl(cam_frame&) = 0;
		virtual void feed_accel_nonbl(accel&) = 0;
		virtual pose& produce_nonbl() = 0;
		virtual ~abstract_slam() { }
	};

}

#define ILLIXR_make_dynamic_factory(abstract_type, implementation) \
	extern "C" abstract_type* make_##abstract_type() { return new implementation; }
