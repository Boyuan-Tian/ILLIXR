#include "common/phonebook.hpp"
#include "common/pose_prediction.hpp"
#include "common/data_format.hpp"
#include "common/plugin.hpp"

using namespace ILLIXR;

class pose_prediction_impl : public pose_prediction {
public:
    pose_prediction_impl(const phonebook* const pb)
		: sb{pb->lookup_impl<switchboard>()}
		, _m_pose{sb->get_reader<pose_type>("slow_pose")}
        , _m_true_pose{sb->get_reader<pose_type>("true_pose")}
    { }

    virtual pose_type get_fast_pose() const override {
		ptr<const pose_type> pose_ptr = _m_pose.get_latest_ro_nullable();
		return correct_pose(
			pose_ptr ? *pose_ptr : pose_type{}
		);
    }

    virtual pose_type get_true_pose() const override {
		ptr<const pose_type> pose_ptr = _m_true_pose.get_latest_ro_nullable();
		return correct_pose(
			pose_ptr ? *pose_ptr : pose_type{}
		);
    }

	virtual void set_offset(const Eigen::Quaternionf& raw_o_times_offset) override {
		std::lock_guard<std::mutex> lock {offset_mutex};
		Eigen::Quaternionf raw_o = raw_o_times_offset * offset.inverse();
		std::cout << "pose_prediction: set_offset" << std::endl;
		offset = raw_o.inverse();
		/*
		  Now, `raw_o` is maps to the identity quaternion.

		  Proof:
		  apply_offset(raw_o)
		      = raw_o * offset
		      = raw_o * raw_o.inverse()
		      = Identity.
		 */
	}

	Eigen::Quaternionf apply_offset(const Eigen::Quaternionf& orientation) const {
		std::lock_guard<std::mutex> lock {offset_mutex};
		return orientation * offset;
	}


	virtual bool fast_pose_reliable() const override {
		//return _m_slow_pose.valid();
		/*
		  SLAM takes some time to initialize, so initially fast_pose
		  is unreliable.

		  In such cases, we might return a fast_pose based only on the
		  IMU data (currently, we just return a zero-pose)., and mark
		  it as "unreliable"

		  This way, there always a pose coming out of pose_prediction,
		  representing our best guess at that time, and we indicate
		  how reliable that guess is here.

		 */
		return bool(_m_pose.get_latest_ro_nullable());
	}

	virtual bool true_pose_reliable() const override {
		//return _m_true_pose.valid();
		/*
		  We do not have a "ground truth" available in all cases, such
		  as when reading live data.
		 */
		return true;
	}

private:
	const std::shared_ptr<switchboard> sb;
    switchboard::reader<pose_type> _m_pose;
    switchboard::reader<pose_type> _m_true_pose;
	Eigen::Quaternionf offset {Eigen::Quaternionf::Identity()};
	mutable std::mutex offset_mutex;

    pose_type correct_pose(const pose_type pose) const {
        pose_type swapped_pose {pose};

        // This uses the OpenVINS standard output coordinate system.
        // This is a mapping between the OV coordinate system and the OpenGL system.
        swapped_pose.position.x() = -pose.position.y();
        swapped_pose.position.y() = pose.position.z();
        swapped_pose.position.z() = -pose.position.x();

		
        // There is a slight issue with the orientations: basically,
        // the output orientation acts as though the "top of the head" is the
        // forward direction, and the "eye direction" is the up direction.
		Eigen::Quaternionf raw_o (pose.orientation.w(), -pose.orientation.y(), pose.orientation.z(), -pose.orientation.x());

		swapped_pose.orientation = apply_offset(raw_o);

        return swapped_pose;
    }
};

class pose_prediction_plugin : public plugin {
public:
    pose_prediction_plugin(const std::string& name, phonebook* pb)
    	: plugin{name, pb}
	{
		pb->register_impl<pose_prediction>(
			std::static_pointer_cast<pose_prediction>(
				std::make_shared<pose_prediction_impl>(pb)
			)
		);
	}
};

PLUGIN_MAIN(pose_prediction_plugin);
