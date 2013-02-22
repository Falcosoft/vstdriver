#ifndef _DS_STREAM_H_
#define _DS_STREAM_H_

class ds_stream
{
public:
	ds_stream() {}
	virtual ~ds_stream() {}

	virtual bool write(const void * data,unsigned bytes)=0;
	virtual bool is_playing()=0;
	virtual double get_latency()=0;
	virtual unsigned get_latency_bytes()=0;
	virtual unsigned can_write_bytes()=0;
	virtual bool force_play()=0;
	virtual bool pause(bool status)=0;
	virtual bool set_ratio(double ratio)=0;
	
	//destructor methods
	virtual void release()=0;
};

struct ds_stream_config
{
	unsigned srate;
	unsigned short nch,bps;
	unsigned buffer_ms;
	
	ds_stream_config()
		: srate(0), nch(0), bps(0), buffer_ms(1000)
	{}
};

class ds_api
{
protected:
	ds_api() {}
	virtual ~ds_api() {}
public:
	virtual ds_stream * ds_stream_create(const ds_stream_config * cfg)=0;
	virtual void set_device(const GUID * id)=0;

	//destructor
	virtual void release()=0;
};

ds_api * ds_api_create(HWND appwindow);

#endif
