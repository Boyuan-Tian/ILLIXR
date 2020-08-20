#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "common/threadloop.hpp"
#include "common/switchboard.hpp"
#include "common/data_format.hpp"
#include "common/extended_window.hpp"
#include "common/shader_util.hpp"
#include "utils/algebra.hpp"
#include "utils/hmd.hpp"
#include "shaders/basic_shader.hpp"
#include "shaders/timewarp_shader.hpp"
#include "common/linalg.hpp"
#include "common/pose_prediction.hpp"

// For generating the output images
#include <stdio.h>
#include <cstdio>

// The dimension for the output images
#define width 800
#define height 600

using namespace ILLIXR;
using namespace linalg::aliases;

typedef void (*glXSwapIntervalEXTProc)(Display *dpy, GLXDrawable drawable, int interval);

// If this is defined, gldemo will use Monado-style eyebuffers
//#define USE_ALT_EYE_FORMAT

const record_header timewarp_gpu_record {
	"timewarp_gpu",
	{
		{"iteration_no", typeid(std::size_t)},
		{"wall_time_start", typeid(std::chrono::high_resolution_clock::time_point)},
		{"wall_time_stop" , typeid(std::chrono::high_resolution_clock::time_point)},
		{"gpu_time_duration", typeid(std::chrono::nanoseconds)},
	},
};

class timewarp_gl : public threadloop {

public:
	// Public constructor, create_component passes Switchboard handles ("plugs")
	// to this constructor. In turn, the constructor fills in the private
	// references to the switchboard plugs, so the component can read the
	// data whenever it needs to.
	timewarp_gl(std::string name_, phonebook* pb_)
		: threadloop{name_, pb_}
		, sb{pb->lookup_impl<switchboard>()}
		, pp{pb->lookup_impl<pose_prediction>()}
		, xwin{pb->lookup_impl<xlib_gl_extended_window>()}
	#ifdef USE_ALT_EYE_FORMAT
		, _m_eyebuffer{sb->subscribe_latest<rendered_frame_alt>("eyebuffer")}
	#else
		, _m_eyebuffer{sb->subscribe_latest<rendered_frame>("eyebuffer")}
	#endif
		, _m_hologram{sb->publish<hologram_input>("hologram_in")}
		, _m_vsync_estimate{sb->publish<time_type>("vsync_estimate")}		
	{
		startTime = std::chrono::system_clock::now();
	}

private:
	const std::shared_ptr<switchboard> sb;
	const std::shared_ptr<pose_prediction> pp;

	static constexpr int   SCREEN_WIDTH    = 2560;
	static constexpr int   SCREEN_HEIGHT   = 1440;

	static constexpr double DISPLAY_REFRESH_RATE = 60.0;
	static constexpr double FPS_WARNING_TOLERANCE = 0.5;
	static constexpr double DELAY_FRACTION = 0.8;

	static constexpr double RUNNING_AVG_ALPHA = 0.1;

	static constexpr std::chrono::nanoseconds vsync_period {std::size_t(NANO_SEC/DISPLAY_REFRESH_RATE)};

	const std::shared_ptr<xlib_gl_extended_window> xwin;
	rendered_frame frame;

	time_type startTime;

	// Switchboard plug for application eye buffer.
	#ifdef USE_ALT_EYE_FORMAT
	std::unique_ptr<reader_latest<rendered_frame_alt>> _m_eyebuffer;
	#else
	std::unique_ptr<reader_latest<rendered_frame>> _m_eyebuffer;
	#endif

	// Switchboard plug for sending hologram calls
	std::unique_ptr<writer<hologram_input>> _m_hologram;

	// Switchboard plug for publishing 
	std::unique_ptr<writer<time_type>> _m_vsync_estimate;

	GLuint timewarpShaderProgram;

	time_type lastSwapTime;
	GLuint64 total_gpu_time = 0;

	HMD::hmd_info_t hmd_info;
	HMD::body_info_t body_info;

	// Eye sampler array
	GLuint eye_sampler_0;
	GLuint eye_sampler_1;

	// Eye index uniform
	GLuint tw_eye_index_unif;

	// VAOs
	GLuint tw_vao;

	// Position and UV attribute locations
	GLuint distortion_pos_attr;
	GLuint distortion_uv0_attr;
	GLuint distortion_uv1_attr;
	GLuint distortion_uv2_attr;

	// Distortion mesh information
	GLuint num_distortion_vertices;
	GLuint num_distortion_indices;

	// Distortion mesh CPU buffers and GPU VBO handles
	HMD::mesh_coord3d_t* distortion_positions;
	GLuint distortion_positions_vbo;
	GLuint* distortion_indices;
	GLuint distortion_indices_vbo;
	HMD::uv_coord_t* distortion_uv0;
	GLuint distortion_uv0_vbo;
	HMD::uv_coord_t* distortion_uv1;
	GLuint distortion_uv1_vbo;
	HMD::uv_coord_t* distortion_uv2;
	GLuint distortion_uv2_vbo;

	// Handles to the start and end timewarp
	// transform matrices (3x4 uniforms)
	GLuint tw_start_transform_unif;
	GLuint tw_end_transform_unif;
	// Basic perspective projection matrix
	ksAlgebra::ksMatrix4x4f basicProjection;

	// Hologram call data
	long long _hologram_seq{0};

	void BuildTimewarp(HMD::hmd_info_t* hmdInfo){

		// Calculate the number of vertices+indices in the distortion mesh.
		num_distortion_vertices = ( hmdInfo->eyeTilesHigh + 1 ) * ( hmdInfo->eyeTilesWide + 1 );
		num_distortion_indices = hmdInfo->eyeTilesHigh * hmdInfo->eyeTilesWide * 6;

		// Allocate memory for the elements/indices array.
		distortion_indices = (GLuint*) malloc(num_distortion_indices * sizeof(GLuint));

		// This is just a simple grid/plane index array, nothing fancy.
		// Same for both eye distortions, too!
		for ( int y = 0; y < hmdInfo->eyeTilesHigh; y++ )
		{
			for ( int x = 0; x < hmdInfo->eyeTilesWide; x++ )
			{
				const int offset = ( y * hmdInfo->eyeTilesWide + x ) * 6;

				distortion_indices[offset + 0] = (GLuint)( ( y + 0 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 0 ) );
				distortion_indices[offset + 1] = (GLuint)( ( y + 1 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 0 ) );
				distortion_indices[offset + 2] = (GLuint)( ( y + 0 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 1 ) );

				distortion_indices[offset + 3] = (GLuint)( ( y + 0 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 1 ) );
				distortion_indices[offset + 4] = (GLuint)( ( y + 1 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 0 ) );
				distortion_indices[offset + 5] = (GLuint)( ( y + 1 ) * ( hmdInfo->eyeTilesWide + 1 ) + ( x + 1 ) );
			}
		}

		// Allocate memory for the distortion coordinates.
		// These are NOT the actual distortion mesh's vertices,
		// they are calculated distortion grid coefficients
		// that will be used to set the actual distortion mesh's UV space.
		HMD::mesh_coord2d_t* tw_mesh_base_ptr = (HMD::mesh_coord2d_t *) malloc( HMD::NUM_EYES * HMD::NUM_COLOR_CHANNELS * num_distortion_vertices * sizeof( HMD::mesh_coord2d_t ) );

		// Set the distortion coordinates as a series of arrays
		// that will be written into by the BuildDistortionMeshes() function.
		HMD::mesh_coord2d_t * distort_coords[HMD::NUM_EYES][HMD::NUM_COLOR_CHANNELS] =
		{
			{ tw_mesh_base_ptr + 0 * num_distortion_vertices, tw_mesh_base_ptr + 1 * num_distortion_vertices, tw_mesh_base_ptr + 2 * num_distortion_vertices },
			{ tw_mesh_base_ptr + 3 * num_distortion_vertices, tw_mesh_base_ptr + 4 * num_distortion_vertices, tw_mesh_base_ptr + 5 * num_distortion_vertices }
		};
		HMD::BuildDistortionMeshes( distort_coords, hmdInfo );

		// Allocate memory for position and UV CPU buffers.
		for(int eye = 0; eye < HMD::NUM_EYES; eye++){
			distortion_positions = (HMD::mesh_coord3d_t *) malloc(HMD::NUM_EYES * num_distortion_vertices * sizeof(HMD::mesh_coord3d_t));
			distortion_uv0 = (HMD::uv_coord_t *) malloc(HMD::NUM_EYES * num_distortion_vertices * sizeof(HMD::uv_coord_t));
			distortion_uv1 = (HMD::uv_coord_t *) malloc(HMD::NUM_EYES * num_distortion_vertices * sizeof(HMD::uv_coord_t));
			distortion_uv2 = (HMD::uv_coord_t *) malloc(HMD::NUM_EYES * num_distortion_vertices * sizeof(HMD::uv_coord_t));
		}

		for ( int eye = 0; eye < HMD::NUM_EYES; eye++ )
		{
			for ( int y = 0; y <= hmdInfo->eyeTilesHigh; y++ )
			{
				for ( int x = 0; x <= hmdInfo->eyeTilesWide; x++ )
				{
					const int index = y * ( hmdInfo->eyeTilesWide + 1 ) + x;

					// Set the physical distortion mesh coordinates. These are rectangular/gridlike, not distorted.
					// The distortion is handled by the UVs, not the actual mesh coordinates!
					distortion_positions[eye * num_distortion_vertices + index].x = ( -1.0f + eye + ( (float)x / hmdInfo->eyeTilesWide ) );
					distortion_positions[eye * num_distortion_vertices + index].y = ( -1.0f + 2.0f * ( ( hmdInfo->eyeTilesHigh - (float)y ) / hmdInfo->eyeTilesHigh ) *
														( (float)( hmdInfo->eyeTilesHigh * hmdInfo->tilePixelsHigh ) / hmdInfo->displayPixelsHigh ) );
					distortion_positions[eye * num_distortion_vertices + index].z = 0.0f;

					// Use the previously-calculated distort_coords to set the UVs on the distortion mesh
					distortion_uv0[eye * num_distortion_vertices + index].u = distort_coords[eye][0][index].x;
					distortion_uv0[eye * num_distortion_vertices + index].v = distort_coords[eye][0][index].y;
					distortion_uv1[eye * num_distortion_vertices + index].u = distort_coords[eye][1][index].x;
					distortion_uv1[eye * num_distortion_vertices + index].v = distort_coords[eye][1][index].y;
					distortion_uv2[eye * num_distortion_vertices + index].u = distort_coords[eye][2][index].x;
					distortion_uv2[eye * num_distortion_vertices + index].v = distort_coords[eye][2][index].y;

				}
			}
		}
		// Construct a basic perspective projection
		ksAlgebra::ksMatrix4x4f_CreateProjectionFov( &basicProjection, 40.0f, 40.0f, 40.0f, 40.0f, 0.1f, 0.0f );

		// This was just temporary.
		free(tw_mesh_base_ptr);

		return;
	}

	/* Calculate timewarm transform from projection matrix, view matrix, etc */
	void CalculateTimeWarpTransform( ksAlgebra::ksMatrix4x4f * transform, const ksAlgebra::ksMatrix4x4f * renderProjectionMatrix,
                                        const ksAlgebra::ksMatrix4x4f * renderViewMatrix, const ksAlgebra::ksMatrix4x4f * newViewMatrix )
	{
		// Convert the projection matrix from [-1, 1] space to [0, 1] space.
		const ksAlgebra::ksMatrix4x4f texCoordProjection =
		{ {
			{ 0.5f * renderProjectionMatrix->m[0][0],        0.0f,                                           0.0f,  0.0f },
			{ 0.0f,                                          0.5f * renderProjectionMatrix->m[1][1],         0.0f,  0.0f },
			{ 0.5f * renderProjectionMatrix->m[2][0] - 0.5f, 0.5f * renderProjectionMatrix->m[2][1] - 0.5f, -1.0f,  0.0f },
			{ 0.0f,                                          0.0f,                                           0.0f,  1.0f }
		} };

		// Calculate the delta between the view matrix used for rendering and
		// a more recent or predicted view matrix based on new sensor input.
		ksAlgebra::ksMatrix4x4f inverseRenderViewMatrix;
		ksAlgebra::ksMatrix4x4f_InvertHomogeneous( &inverseRenderViewMatrix, renderViewMatrix );

		ksAlgebra::ksMatrix4x4f deltaViewMatrix;
		ksAlgebra::ksMatrix4x4f_Multiply( &deltaViewMatrix, &inverseRenderViewMatrix, newViewMatrix );

		ksAlgebra::ksMatrix4x4f inverseDeltaViewMatrix;
		ksAlgebra::ksMatrix4x4f_InvertHomogeneous( &inverseDeltaViewMatrix, &deltaViewMatrix );

		// Make the delta rotation only.
		inverseDeltaViewMatrix.m[3][0] = 0.0f;
		inverseDeltaViewMatrix.m[3][1] = 0.0f;
		inverseDeltaViewMatrix.m[3][2] = 0.0f;

		// TODO: Major issue. The original implementation uses inverseDeltaMatrix... as expected.
		// This is because we are applying the /inverse/ of the delta between the original render
		// view matrix and the new latepose. However, when used the inverse, the transformation
		// was actually backwards. I'm not sure if this is an issue in the demo app/rendering thread,
		// or if there's something messed up with the ATW code. In any case, using the actual
		// delta matrix itself yields the correct result, which is very odd. Must investigate!

		deltaViewMatrix.m[3][0] = 0.0f;
		deltaViewMatrix.m[3][1] = 0.0f;
		deltaViewMatrix.m[3][2] = 0.0f;

		// Accumulate the transforms.
		//ksAlgebra::ksMatrix4x4f_Multiply( transform, &texCoordProjection, &inverseDeltaViewMatrix );
		ksAlgebra::ksMatrix4x4f_Multiply( transform, &texCoordProjection, &deltaViewMatrix );
	}

	// Get the estimated time of the next swap/next Vsync.
	// This is an estimate, used to wait until *just* before vsync.
	time_type GetNextSwapTimeEstimate(){
		return lastSwapTime + vsync_period;
	}

	// Get the estimated amount of time to put the CPU thread to sleep,
	// given a specified percentage of the total Vsync period to delay.
	std::chrono::duration<double, std::nano> EstimateTimeToSleep(double framePercentage){
		return (GetNextSwapTimeEstimate() - std::chrono::high_resolution_clock::now()) * framePercentage;
	}


public:

	virtual skip_option _p_should_skip() override {
		using namespace std::chrono_literals;
		// Sleep for approximately 90% of the time until the next vsync.
		// Scheduling granularity can't be assumed to be super accurate here,
		// so don't push your luck (i.e. don't wait too long....) Tradeoff with
		// MTP here. More you wait, closer to the display sync you sample the pose.

		// TODO: poll GLX window events
		std::this_thread::sleep_for(std::chrono::duration<double>(EstimateTimeToSleep(DELAY_FRACTION)));
		if(_m_eyebuffer->get_latest_ro()) {
			return skip_option::run;
		} else {
			// Null means system is nothing has been pushed yet
			// because not all components are initialized yet
			return skip_option::skip_and_yield;
		}
	}

	virtual void _p_one_iteration() override {
		warp(glfwGetTime());
	}

	virtual void _p_thread_setup() override {
		lastSwapTime = std::chrono::high_resolution_clock::now();

		// Generate reference HMD and physical body dimensions
    	HMD::GetDefaultHmdInfo(SCREEN_WIDTH, SCREEN_HEIGHT, &hmd_info);
		HMD::GetDefaultBodyInfo(&body_info);

		// Initialize the GLFW library, still need it to get time
		if(!glfwInit()){
			printf("Failed to initialize glfw\n");
		}

    	// Construct timewarp meshes and other data
    	BuildTimewarp(&hmd_info);

		// includes setting swap interval
		glXMakeCurrent(xwin->dpy, xwin->win, xwin->glc);

		// set swap interval for 1
		glXSwapIntervalEXTProc glXSwapIntervalEXT = 0;
		glXSwapIntervalEXT = (glXSwapIntervalEXTProc) glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
		glXSwapIntervalEXT(xwin->dpy, xwin->win, 1);

		// Init and verify GLEW
		glewExperimental = GL_TRUE;
		if(glewInit() != GLEW_OK){
			printf("Failed to init GLEW\n");
			// clean up ?
			exit(0);
		}

		glEnable              ( GL_DEBUG_OUTPUT );
		glDebugMessageCallback( MessageCallback, 0 );

		// TODO: X window v-synch

		// Create and bind global VAO object
		glGenVertexArrays(1, &tw_vao);
    	glBindVertexArray(tw_vao);

    	#ifdef USE_ALT_EYE_FORMAT
    	timewarpShaderProgram = init_and_link(timeWarpChromaticVertexProgramGLSL, timeWarpChromaticFragmentProgramGLSL_Alternative);
    	#else
		timewarpShaderProgram = init_and_link(timeWarpChromaticVertexProgramGLSL, timeWarpChromaticFragmentProgramGLSL);
		#endif
		// Acquire attribute and uniform locations from the compiled and linked shader program

    	distortion_pos_attr = glGetAttribLocation(timewarpShaderProgram, "vertexPosition");
    	distortion_uv0_attr = glGetAttribLocation(timewarpShaderProgram, "vertexUv0");
    	distortion_uv1_attr = glGetAttribLocation(timewarpShaderProgram, "vertexUv1");
    	distortion_uv2_attr = glGetAttribLocation(timewarpShaderProgram, "vertexUv2");
    	tw_start_transform_unif = glGetUniformLocation(timewarpShaderProgram, "TimeWarpStartTransform");
    	tw_end_transform_unif = glGetUniformLocation(timewarpShaderProgram, "TimeWarpEndTransform");
    	tw_eye_index_unif = glGetUniformLocation(timewarpShaderProgram, "ArrayLayer");
    	eye_sampler_0 = glGetUniformLocation(timewarpShaderProgram, "Texture[0]");
    	eye_sampler_1 = glGetUniformLocation(timewarpShaderProgram, "Texture[1]");



		// Config distortion mesh position vbo
		glGenBuffers(1, &distortion_positions_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, distortion_positions_vbo);
		glBufferData(GL_ARRAY_BUFFER, HMD::NUM_EYES * (num_distortion_vertices * 3) * sizeof(GLfloat), distortion_positions, GL_STATIC_DRAW);
		glVertexAttribPointer(distortion_pos_attr, 3, GL_FLOAT, GL_FALSE, 0, 0);
		//glEnableVertexAttribArray(distortion_pos_attr);



		// Config distortion uv0 vbo
		glGenBuffers(1, &distortion_uv0_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, distortion_uv0_vbo);
		glBufferData(GL_ARRAY_BUFFER, HMD::NUM_EYES * (num_distortion_vertices * 2) * sizeof(GLfloat), distortion_uv0, GL_STATIC_DRAW);
		glVertexAttribPointer(distortion_uv0_attr, 2, GL_FLOAT, GL_FALSE, 0, 0);
		//glEnableVertexAttribArray(distortion_uv0_attr);


		// Config distortion uv1 vbo
		glGenBuffers(1, &distortion_uv1_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, distortion_uv1_vbo);
		glBufferData(GL_ARRAY_BUFFER, HMD::NUM_EYES * (num_distortion_vertices * 2) * sizeof(GLfloat), distortion_uv1, GL_STATIC_DRAW);
		glVertexAttribPointer(distortion_uv1_attr, 2, GL_FLOAT, GL_FALSE, 0, 0);
		//glEnableVertexAttribArray(distortion_uv1_attr);


		// Config distortion uv2 vbo
		glGenBuffers(1, &distortion_uv2_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, distortion_uv2_vbo);
		glBufferData(GL_ARRAY_BUFFER, HMD::NUM_EYES * (num_distortion_vertices * 2) * sizeof(GLfloat), distortion_uv2, GL_STATIC_DRAW);
		glVertexAttribPointer(distortion_uv2_attr, 2, GL_FLOAT, GL_FALSE, 0, 0);
		//glEnableVertexAttribArray(distortion_uv2_attr);


		// Config distortion mesh indices vbo
		glGenBuffers(1, &distortion_indices_vbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, distortion_indices_vbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, num_distortion_indices * sizeof(GLuint), distortion_indices, GL_STATIC_DRAW);

		glXMakeCurrent(xwin->dpy, None, NULL);
	}


	void GetViewMatrixFromPose( ksAlgebra::ksMatrix4x4f* viewMatrix, const pose_type& pose) {
		// Cast from the "standard" quaternion to our own, proprietary, Oculus-flavored quaternion
		auto latest_quat = ksAlgebra::ksQuatf {
			.x = pose.orientation.x(),
			.y = pose.orientation.y(),
			.z = pose.orientation.z(),
			.w = pose.orientation.w()
		};
		ksAlgebra::ksMatrix4x4f_CreateFromQuaternion( viewMatrix, &latest_quat);
	}

	virtual void warp([[maybe_unused]] float time) {
		glXMakeCurrent(xwin->dpy, xwin->win, xwin->glc);

		glBindFramebuffer(GL_FRAMEBUFFER,0);
		glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		glClearColor(0, 0, 0, 0);
    	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glDepthFunc(GL_LEQUAL);

		auto most_recent_frame = _m_eyebuffer->get_latest_ro();
		// This should be null-checked in _p_should_skip
		assert(most_recent_frame);

		// Use the timewarp program
		glUseProgram(timewarpShaderProgram);

#ifndef NDEBUG
		// double warpStart = glfwGetTime();
#endif
		//double cursor_x, cursor_y;
		//glfwGetCursorPos(window, &cursor_x, &cursor_y);

		// Generate "starting" view matrix, from the pose
		// sampled at the time of rendering the frame.
		ksAlgebra::ksMatrix4x4f viewMatrix;
		GetViewMatrixFromPose(&viewMatrix, most_recent_frame->render_pose.pose);

		// We simulate two asynchronous view matrices,
		// one at the beginning of display refresh,
		// and one at the end of display refresh.
		// The distortion shader will lerp between
		// these two predictive view transformations
		// as it renders across the horizontal view,
		// compensating for display panel refresh delay (wow!)
		ksAlgebra::ksMatrix4x4f viewMatrixBegin;
		ksAlgebra::ksMatrix4x4f viewMatrixEnd;

		// TODO: Right now, this samples the latest pose published to the "pose" topic.
		// However, this should really be polling the high-frequency pose prediction topic,
		// given a specified timestamp!
		const pose_type latest_pose = pp->get_fast_pose().pose;
		GetViewMatrixFromPose(&viewMatrixBegin, latest_pose);

		// std::cout << "Timewarp: old " << most_recent_frame->render_pose.pose << ", new " << latest_pose->pose << std::endl;

		// TODO: We set the "end" pose to the same as the beginning pose, because panel refresh is so tiny
		// and we don't need to visualize this right now (we also don't have prediction setup yet!)
		viewMatrixEnd = viewMatrixBegin;

		// Get HMD view matrices, one for the beginning of the
		// panel refresh, one for the end. (This is set to a 0.1s panel
		// refresh duration, for exaggerated effect)
		//GetHmdViewMatrixForTime(&viewMatrixBegin, glfwGetTime());
		//GetHmdViewMatrixForTime(&viewMatrixEnd, glfwGetTime() + 0.1f);

		//ksAlgebra::ksMatrix4x4f_CreateRotation( &viewMatrixBegin, (cursor_y - SCREEN_HEIGHT/2) * 0.05, (cursor_x - SCREEN_WIDTH/2) * 0.05, 0.0f );
		//ksAlgebra::ksMatrix4x4f_CreateRotation( &viewMatrixEnd, (cursor_y - SCREEN_HEIGHT/2) * 0.05, (cursor_x - SCREEN_WIDTH/2) * 0.05, 0.0f );

		// Calculate the timewarp transformation matrices.
		// These are a product of the last-known-good view matrix
		// and the predictive transforms.
		ksAlgebra::ksMatrix4x4f timeWarpStartTransform4x4;
		ksAlgebra::ksMatrix4x4f timeWarpEndTransform4x4;

		// DEMONSTRATION:
		// Every second, toggle timewarp on and off
		// to show the effect of the reprojection.
		/*
		if(glfwGetTime() < 9.0){
			viewMatrixBegin = viewMatrix;
			viewMatrixEnd = viewMatrix;
		}
		*/

		// Calculate timewarp transforms using predictive view transforms
		CalculateTimeWarpTransform(&timeWarpStartTransform4x4, &basicProjection, &viewMatrix, &viewMatrixBegin);
		CalculateTimeWarpTransform(&timeWarpEndTransform4x4, &basicProjection, &viewMatrix, &viewMatrixEnd);

		// We transform from 4x4 to 3x4 as we operate on vec3's in NDC space
		ksAlgebra::ksMatrix3x4f timeWarpStartTransform3x4;
		ksAlgebra::ksMatrix3x4f timeWarpEndTransform3x4;
		ksAlgebra::ksMatrix3x4f_CreateFromMatrix4x4f( &timeWarpStartTransform3x4, &timeWarpStartTransform4x4 );
		ksAlgebra::ksMatrix3x4f_CreateFromMatrix4x4f( &timeWarpEndTransform3x4, &timeWarpEndTransform4x4 );

		// Push timewarp transform matrices to timewarp shader
		glUniformMatrix3x4fv(tw_start_transform_unif, 1, GL_FALSE, (GLfloat*)&(timeWarpStartTransform3x4.m[0][0]));
		glUniformMatrix3x4fv(tw_end_transform_unif, 1, GL_FALSE,  (GLfloat*)&(timeWarpEndTransform3x4.m[0][0]));

		// Debugging aid, toggle switch for rendering in the fragment shader
		glUniform1i(glGetUniformLocation(timewarpShaderProgram, "ArrayIndex"), 0);

		glUniform1i(eye_sampler_0, 0);

		#ifndef USE_ALT_EYE_FORMAT
		// Bind the shared texture handle
		glBindTexture(GL_TEXTURE_2D_ARRAY, most_recent_frame->texture_handle);
		#endif

		glBindVertexArray(tw_vao);

		auto gpu_start_wall_time = std::chrono::high_resolution_clock::now();

		GLuint query;
		GLuint64 elapsed_time = 0;
		glGenQueries(1, &query);
		glBeginQuery(GL_TIME_ELAPSED, query);

		// Loop over each eye.
		for(int eye = 0; eye < HMD::NUM_EYES; eye++){

			#ifdef USE_ALT_EYE_FORMAT // If we're using Monado-style buffers we need to rebind eyebuffers.... eugh!
			glBindTexture(GL_TEXTURE_2D, most_recent_frame->texture_handles[eye]);
			#endif

			// The distortion_positions_vbo GPU buffer already contains
			// the distortion mesh for both eyes! They are contiguously
			// laid out in GPU memory. Therefore, on each eye render,
			// we set the attribute pointer to be offset by the full
			// eye's distortion mesh size, rendering the correct eye mesh
			// to that region of the screen. This prevents re-uploading
			// GPU data for each eye.
			glBindBuffer(GL_ARRAY_BUFFER, distortion_positions_vbo);
			glVertexAttribPointer(distortion_pos_attr, 3, GL_FLOAT, GL_FALSE, 0, (void*)(eye * num_distortion_vertices * sizeof(HMD::mesh_coord3d_t)));
			glEnableVertexAttribArray(distortion_pos_attr);

			// We do the exact same thing for the UV GPU memory.
			glBindBuffer(GL_ARRAY_BUFFER, distortion_uv0_vbo);
			glVertexAttribPointer(distortion_uv0_attr, 2, GL_FLOAT, GL_FALSE, 0, (void*)(eye * num_distortion_vertices * sizeof(HMD::mesh_coord2d_t)));
			glEnableVertexAttribArray(distortion_uv0_attr);

			// We do the exact same thing for the UV GPU memory.
			glBindBuffer(GL_ARRAY_BUFFER, distortion_uv1_vbo);
			glVertexAttribPointer(distortion_uv1_attr, 2, GL_FLOAT, GL_FALSE, 0, (void*)(eye * num_distortion_vertices * sizeof(HMD::mesh_coord2d_t)));
			glEnableVertexAttribArray(distortion_uv1_attr);

			// We do the exact same thing for the UV GPU memory.
			glBindBuffer(GL_ARRAY_BUFFER, distortion_uv2_vbo);
			glVertexAttribPointer(distortion_uv2_attr, 2, GL_FLOAT, GL_FALSE, 0, (void*)(eye * num_distortion_vertices * sizeof(HMD::mesh_coord2d_t)));
			glEnableVertexAttribArray(distortion_uv2_attr);


			#ifndef USE_ALT_EYE_FORMAT // If we are using normal ILLIXR-format eyebuffers
			// Specify which layer of the eye texture we're going to be using.
			// Each eye has its own layer.
			glUniform1i(tw_eye_index_unif, eye);
			#endif

			// Interestingly, the element index buffer is identical for both eyes, and is
			// reused for both eyes. Therefore glDrawElements can be immediately called,
			// with the UV and position buffers correctly offset.
			glDrawElements(GL_TRIANGLES, num_distortion_indices, GL_UNSIGNED_INT, (void*)0);

			// The following loop is used to generate the images from the textures we obtained before
			if (eye == 0) {
				GLuint fb = 1;
				glGenFramebuffers(1, &fb);
				glBindFramebuffer(GL_FRAMEBUFFER, fb);
				char addr[50];
				sprintf(addr, "./ideal-new/eye/left/%ld_timestamp.ppm", ((GetNextSwapTimeEstimate() - startTime).count() / 1000));
				FILE* out = fopen(addr, "wb");
				
				unsigned char* pixels = (unsigned char*)malloc(width * height * 3);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, most_recent_frame->texture_handles[0], 0);
				
				glReadBuffer(GL_COLOR_ATTACHMENT0);
				
				glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
				
				fprintf(out,"P3\n");
				
				fprintf(out,"# Created by the ILLIXR team\n");
				
				fprintf(out,"%d %d\n",width, height);
				fprintf(out,"255\n");

				int k = 0;
				
				for (int i = 0; i < width; ++i) {
        			for(int j = 0; j < height; ++j) {
						// k = (j + i * height) * 3;
            			fprintf(out, "%u %u %u ", (unsigned int) pixels[k], (unsigned int) pixels[k + 1], (unsigned int) pixels[k + 2]);
            			k = k + 3;
        			}
        			fprintf(out,"\n");
    			}
    			free(pixels);

				fclose(out);
			}
		}

		glEndQuery(GL_TIME_ELAPSED);

#ifndef NDEBUG
		auto delta = std::chrono::high_resolution_clock::now() - most_recent_frame->render_time;
		printf("\033[1;36m[TIMEWARP]\033[0m Time since render: %3fms\n", (float)(delta.count() / 1000000.0));
		if(delta > vsync_period)
		{
			printf("\033[0;31m[TIMEWARP: CRITICAL]\033[0m Stale frame!\n");
		}
		printf("\033[1;36m[TIMEWARP]\033[0m Warping from swap %d\n", most_recent_frame->swap_indices[0]);
#endif
		// Call Hologram
		auto hologram_params = new hologram_input;
		hologram_params->seq = ++_hologram_seq;
		_m_hologram->put(hologram_params);

		// Call swap buffers; when vsync is enabled, this will return to the CPU thread once the buffers have been successfully swapped.
		// TODO: GLX V SYNCH SWAP BUFFER
		auto beforeSwap = glfwGetTime();
		glXSwapBuffers(xwin->dpy, xwin->win);
		auto afterSwap = glfwGetTime();

#ifndef NDEBUG
		printf("\033[1;36m[TIMEWARP]\033[0m Swap time: %5fms\n", (float)(afterSwap - beforeSwap) * 1000);
#endif

		// retrieving the recorded elapsed time
		// wait until the query result is available
		int done = 0;
		glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &done);
		while (!done) {
			std::this_thread::yield();
			glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &done);
		}

		// get the query result
		glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed_time);
		total_gpu_time += elapsed_time;
		metric_logger->log(record{&timewarp_gpu_record, {
			{iteration_no},
			{gpu_start_wall_time},
			{std::chrono::high_resolution_clock::now()},
			{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(total_gpu_time))},
		}});

		lastSwapTime = std::chrono::system_clock::now();

		// Now that we have the most recent swap time, we can publish the new estimate.
		_m_vsync_estimate->put(new time_type(GetNextSwapTimeEstimate()));
#ifndef NDEBUG

		// TODO (implement-logging): When we have logging infra, delete this code.
		// This only looks at warp time, so doesn't take into account IMU frequency.
		// printf("\033[1;36m[TIMEWARP]\033[0m Motion-to-display latency: %.1f ms, Exponential Average FPS: %.3f\n", (float)(lastSwapTime - warpStart) * 1000.0f, (float)(averageFramerate));

		std::cout<< "Timewarp estimating: " << std::chrono::duration_cast<std::chrono::milliseconds>(GetNextSwapTimeEstimate() - lastSwapTime).count() << "ms in the future" << std::endl;
#endif
	}

	virtual ~timewarp_gl() override {
		// TODO: Need to cleanup resources here!
		glXMakeCurrent(xwin->dpy, None, NULL);
 		glXDestroyContext(xwin->dpy, xwin->glc);
 		XDestroyWindow(xwin->dpy, xwin->win);
 		XCloseDisplay(xwin->dpy);
	}
};

PLUGIN_MAIN(timewarp_gl)