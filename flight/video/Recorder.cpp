#include <dirent.h>
#include <string.h>
#include "Recorder.h"
#include <Board.h>

Recorder::Recorder()
	: Thread( "Recorder" )
	, mRecordId( 0 )
	, mRecordSyncCounter( 0 )
{
	uint32_t fileid = 0;
	DIR* dir;
	struct dirent* ent;
	if ( ( dir = opendir( "/var/VIDEO" ) ) != nullptr ) {
		while ( ( ent = readdir( dir ) ) != nullptr ) {
			std::string file = std::string( ent->d_name );
			if ( file.find( "record_" ) != file.npos ) {
				uint32_t id = std::atoi( file.substr( file.rfind( "_" ) + 1 ).c_str() );
				if ( id >= fileid ) {
					fileid = id + 1;
				}
			}
		}
		closedir( dir );
	}
	mRecordId = fileid;

	char filename[256];
	sprintf( filename, "/var/VIDEO/record_%06u.csv", mRecordId );
	mRecordFile = fopen( filename, "wb" );
	fprintf( mRecordFile, "# new_track,track_id,type(video/audio),filename\n" );
	fprintf( mRecordFile, "# track_id,record_time,pos_in_file,frame_size\n" );

	Start();
}


Recorder::~Recorder()
{
}


uint32_t Recorder::AddVideoTrack( uint32_t width, uint32_t height, uint32_t average_fps, const std::string& extension )
{
	Track* track = new Track;
	track->type = TrackTypeVideo;
	sprintf( track->filename, "video_%ux%u_%02ufps_%06u.%s", width, height, average_fps, mRecordId, extension.c_str() );
	track->file = fopen( ( std::string("/var/VIDEO/") + std::string(track->filename) ).c_str(), "wb" );

	mWriteMutex.lock();
	track->id = mTracks.size();
	fprintf( mRecordFile, "new_track,%u,video,%s\n", track->id, track->filename );
	mTracks.emplace_back( track );
	mWriteMutex.unlock();

	return track->id;
}



uint32_t Recorder::AddAudioTrack( uint32_t channels, uint32_t sample_rate, const std::string& extension )
{
	Track* track = new Track;
	track->type = TrackTypeAudio;
	sprintf( track->filename, "audio_%uhz_%uch_%06u.%s", sample_rate, channels, mRecordId, extension.c_str() );
	track->file = fopen( ( std::string("/var/VIDEO/") + std::string(track->filename) ).c_str(), "wb" );

	mWriteMutex.lock();
	track->id = mTracks.size();
	fprintf( mRecordFile, "new_track,%u,audio,%s\n", track->id, track->filename );
	mTracks.emplace_back( track );
	mWriteMutex.unlock();

	return track->id;
}


void Recorder::WriteSample( uint32_t track_id, uint64_t record_time_us, void* buf, uint32_t buflen )
{
	PendingSample* sample = new PendingSample;
	sample->track = mTracks.at(track_id);
	sample->record_time_us = record_time_us;
	sample->buf = new uint8_t[buflen];
	sample->buflen = buflen;
	memcpy( sample->buf, buf, buflen );

	mWriteMutex.lock();
	mPendingSamples.emplace_back( sample );
	mWriteMutex.unlock();
}


bool Recorder::run()
{
	// Wait until there is data to write (TBD : use pthread_cond for passive wait ?)
	mWriteMutex.lock();
	if ( mPendingSamples.size() == 0 ) {
		mWriteMutex.unlock();
		usleep( 1000 * 5 ); // wait 5 ms, allowing up to 200FPS video recording
		return true;
	}

	PendingSample* sample = mPendingSamples.front();
	mPendingSamples.pop_front();
	mWriteMutex.unlock();

	uint32_t pos = ftell( sample->track->file );
	if ( fwrite( sample->buf, 1, sample->buflen, sample->track->file ) != sample->buflen ) {
		goto err;
	}
	if ( fprintf( mRecordFile, "%u,%llu,%u,%u\n", sample->track->id, sample->record_time_us, pos, sample->buflen ) <= 0 ) {
		goto err;
	}

	if ( mRecordSyncCounter == 0 ) {
		if ( fflush( mRecordFile ) < 0 or fsync( fileno( mRecordFile ) ) < 0 ) {
			goto err;
		}
	}
	mRecordSyncCounter = ( mRecordSyncCounter + 1 ) % 10; // sync on disk every 10 samples (up to 10*1/FPS seconds)

	goto end;

err:
	if ( errno == ENOSPC ) {
		Board::setDiskFull();
	}

end:
	delete[] sample->buf;
	delete sample;
	return true;
}
