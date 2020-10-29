#include <shared_mutex>
#include <eigen3/Eigen/Dense>
#include "common/phonebook.hpp"
#include "common/mxre.hpp"
#include "common/data_format.hpp"
#include "common/plugin.hpp"

using namespace ILLIXR;

class mxre_impl : public mxre {
public:
    mxre_impl(const phonebook* const pb)
		: sb{pb->lookup_impl<switchboard>()}
        , _m_cam_type{sb->subscribe_latest<imu_cam_type>("imu_cam")}
    { 
        // Init MXRE client and set up communication channels here
    }

    virtual void get_mxre_frame() const override {
        // Call MXRE in here
	}

private:
	const std::shared_ptr<switchboard> sb;
    std::unique_ptr<reader_latest<imu_cam_type>> _m_cam_type;
};

class mxre_plugin : public plugin {
public:
    mxre_plugin(const std::string& name, phonebook* pb)
    	: plugin{name, pb}
	{
		pb->register_impl<mxre>(
			std::static_pointer_cast<mxre>(
				std::make_shared<mxre_impl>(pb)
			)
		);
	}
};

PLUGIN_MAIN(mxre_plugin);
