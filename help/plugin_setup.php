<p>

The SMPTE plugin can be used to generate or respond to Linear Time Codes on an audio channel.
</p>
<p>

When FPP is in Player mode, if the SMPTE plugin is enabled,it will generate time codes based 
on the current playlist and output them on the selected sound device.   The timecode format
can be configured to the various standard "fps" settings (which does not need to match what
FPP is outputting).   If "Hour Field Is Playlist Index" is turned off, the timecode is elapsed
time since the beginning of the playlist.  If it is turned on, the HOUR field of the timecode
is the playlist index and the minutes/seconds/frame is the position in that song.
</p>
<p>

In remote mote, FPP can listen for Linear Time Codes on the selected audio capture device
and sync a local playlist with those codes.  
</p>