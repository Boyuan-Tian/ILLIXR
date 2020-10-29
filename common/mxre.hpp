#include "phonebook.hpp"
#include "data_format.hpp"

using namespace ILLIXR;

class mxre : public phonebook::service {
public:
	virtual void get_mxre_frame() const = 0;
};
