//using System;
using System.Runtime.CompilerServices;
using Tamir.Streams;
using Tamir.SharpSsh.java.io;
using Tamir.SharpSsh.java.lang;
using Tamir.SharpSsh.java.util;
using Tamir.SharpSsh.java;

namespace Tamir.SharpSsh.jsch
{
	/* -*-mode:java; c-basic-offset:2; indent-tabs-mode:nil -*- */
	/*
	Copyright (c) 2002,2003,2004,2005,2006 ymnk, JCraft,Inc. All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright notice,
	this list of conditions and the following disclaimer.

	2. Redistributions in binary form must reproduce the above copyright
	notice, this list of conditions and the following disclaimer in
	the documentation and/or other materials provided with the distribution.

	3. The names of the authors may not be used to endorse or promote products
	derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
	INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
	FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JCRAFT,
	INC. OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
	LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
	OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
	LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
	NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
	EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
	*/

	/// <summary>
	/// Based on JSch-0.1.30
	/// </summary>
	public class ChannelSftp : ChannelSession
	{
		private static  byte SSH_FXP_INIT=               1;
		private static  byte SSH_FXP_VERSION=            2;
		private static  byte SSH_FXP_OPEN=               3;
		private static  byte SSH_FXP_CLOSE=              4;
		private static  byte SSH_FXP_READ=               5;
		private static  byte SSH_FXP_WRITE=              6;
		private static  byte SSH_FXP_LSTAT=              7;
		private static  byte SSH_FXP_FSTAT=              8;
		private static  byte SSH_FXP_SETSTAT=            9;
		private static  byte SSH_FXP_FSETSTAT=          10;
		private static  byte SSH_FXP_OPENDIR=           11;
		private static  byte SSH_FXP_READDIR=           12;
		private static  byte SSH_FXP_REMOVE=            13;
		private static  byte SSH_FXP_MKDIR=             14;
		private static  byte SSH_FXP_RMDIR=             15;
		private static  byte SSH_FXP_REALPATH=          16;
		private static  byte SSH_FXP_STAT=              17;
		private static  byte SSH_FXP_RENAME=            18;
		private static  byte SSH_FXP_READLINK=          19;
		private static  byte SSH_FXP_SYMLINK=           20;
		private static  byte SSH_FXP_STATUS=           101;
		private static  byte SSH_FXP_HANDLE=           102;
		private static  byte SSH_FXP_DATA=             103;
		private static  byte SSH_FXP_NAME=             104;
		private static  byte SSH_FXP_ATTRS=            105;
		private static  byte SSH_FXP_EXTENDED=         (byte)200;
		private static  byte SSH_FXP_EXTENDED_REPLY=   (byte)201;

		// pflags
		private static  int SSH_FXF_READ=           0x00000001;
		private static  int SSH_FXF_WRITE=          0x00000002;
		private static  int SSH_FXF_APPEND=         0x00000004;
		private static  int SSH_FXF_CREAT=          0x00000008;
		private static  int SSH_FXF_TRUNC=          0x00000010;
		private static  int SSH_FXF_EXCL=           0x00000020;

		private static  int SSH_FILEXFER_ATTR_SIZE=         0x00000001;
		private static  int SSH_FILEXFER_ATTR_UIDGID=       0x00000002;
		private static  int SSH_FILEXFER_ATTR_PERMISSIONS=  0x00000004;
		private static  int SSH_FILEXFER_ATTR_ACMODTIME=    0x00000008;
		private static  uint SSH_FILEXFER_ATTR_EXTENDED=     0x80000000;

		public static  int SSH_FX_OK=                            0;
		public static  int SSH_FX_EOF=                           1;
		public static  int SSH_FX_NO_SUCH_FILE=                  2;
		public static  int SSH_FX_PERMISSION_DENIED=             3;
		public static  int SSH_FX_FAILURE=                       4;
		public static  int SSH_FX_BAD_MESSAGE=                   5;
		public static  int SSH_FX_NO_CONNECTION=                 6;
		public static  int SSH_FX_CONNECTION_LOST=               7;
		public static  int SSH_FX_OP_UNSUPPORTED=                8;
		/*
		SSH_FX_OK
		Indicates successful completion of the operation.
		SSH_FX_EOF
		indicates end-of-file condition; for SSH_FX_READ it means that no
		more data is available in the file, and for SSH_FX_READDIR it
		indicates that no more files are contained in the directory.
		SSH_FX_NO_SUCH_FILE
		is returned when a reference is made to a file which should exist
		but doesn't.
		SSH_FX_PERMISSION_DENIED
		is returned when the authenticated user does not have sufficient
		permissions to perform the operation.
		SSH_FX_FAILURE
		is a generic catch-all error message; it should be returned if an
		error occurs for which there is no more specific error code
		defined.
		SSH_FX_BAD_MESSAGE
		may be returned if a badly formatted packet or protocol
		incompatibility is detected.
		SSH_FX_NO_CONNECTION
		is a pseudo-error which indicates that the client has no
		connection to the server (it can only be generated locally by the
		client, and MUST NOT be returned by servers).
		SSH_FX_CONNECTION_LOST
		is a pseudo-error which indicates that the connection to the
		server has been lost (it can only be generated locally by the
		client, and MUST NOT be returned by servers).
		SSH_FX_OP_UNSUPPORTED
		indicates that an attempt was made to perform an operation which
		is not supported for the server (it may be generated locally by
		the client if e.g.  the version number exchange indicates that a
		required feature is not supported by the server, or it may be
		returned by the server if the server does not implement an
		operation).
		*/
		private static  int MAX_MSG_LENGTH = 256* 1024;

		public static  int OVERWRITE=0;
		public static  int RESUME=1;
		public static  int APPEND=2;

		//  private bool interactive=true;
		private bool interactive=false;
		internal int seq=1;
		private int[] ackid=new int[1];
		private Buffer buf;
		private Packet packet;//=new Packet(buf);

		private String _version="3";
		private int server_version=3;
		/*
		10. Changes from previous protocol versions
		The SSH File Transfer Protocol has changed over time, before it's
		standardization.  The following is a description of the incompatible
		changes between different versions.
		10.1 Changes between versions 3 and 2
		o  The SSH_FXP_READLINK and SSH_FXP_SYMLINK messages were added.
		o  The SSH_FXP_EXTENDED and SSH_FXP_EXTENDED_REPLY messages were added.
		o  The SSH_FXP_STATUS message was changed to include fields `error
		message' and `language tag'.
		10.2 Changes between versions 2 and 1
		o  The SSH_FXP_RENAME message was added.
		10.3 Changes between versions 1 and 0
		o  Implementation changes, no actual protocol changes.
		*/

		private static  String file_separator=java.io.File.separator;
		private static  char file_separatorc=java.io.File.separatorChar;

		private String cwd;
		private String home;
		private String lcwd;

		internal ChannelSftp(){packet=new Packet(buf);}

		public override void init()
		{
			/*
			io.setInputStream(session.in);
			io.setOutputStream(session.out);
			*/
		}

		public override void start() 
		{ //throws JSchException{
			try
			{

				PipedOutputStream pos=new PipedOutputStream();
				io.setOutputStream(pos);
				PipedInputStream pis=new MyPipedInputStream(pos, 32*1024);
				io.setInputStream(pis);

				Request request=new RequestSftp();
				request.request(session, this);

				/*
					  System.err.println("lmpsize: "+lmpsize);
					  System.err.println("lwsize: "+lwsize);
					  System.err.println("rmpsize: "+rmpsize);
					  System.err.println("rwsize: "+rwsize);
				*/

				buf=new Buffer(rmpsize);
				packet=new Packet(buf);
				int i=0;
				int length;
				int type;
				byte[] str;

				// send SSH_FXP_INIT
				sendINIT();

				// receive SSH_FXP_VERSION
				Header _header=new Header();
				_header=header(buf, _header);
				length=_header.length;
				if(length > MAX_MSG_LENGTH)
				{
					throw new SftpException(SSH_FX_FAILURE, "Received message is too long: " + length);
				}
				type=_header.type;             // 2 -> SSH_FXP_VERSION
				server_version=_header.rid;
				skip(length);
				//System.err.println("SFTP protocol server-version="+server_version);
				//System.Console.WriteLine("server_version="+server_version+", type="+type+", length="+length+", i="+i);
				
				// send SSH_FXP_REALPATH
				sendREALPATH(new String(".").getBytes());

				// receive SSH_FXP_NAME
				_header=header(buf, _header);
				length=_header.length;
				type=_header.type;            // 104 -> SSH_FXP_NAME
				buf.rewind();
				fill(buf.buffer, 0, length);
				i=buf.getInt();              // count
				//System.Console.WriteLine("type="+type+", length="+length+", i="+i);
				str=buf.getString();         // filename
				//System.Console.WriteLine("str.length="+str.Length);
				home=cwd=new String(str);
				str=buf.getString();         // logname
				//    SftpATTRS.getATTR(buf);      // attrs

				lcwd=new File(".").getCanonicalPath();
			}
			catch(Exception e)
			{
				//System.out.println(e);
				//System.Console.WriteLine(e);
				if(e is JSchException) throw (JSchException)e;
				throw new JSchException(e.toString());
			}
		}

		public void quit(){ disconnect();}
		public void exit(){ disconnect();}
		public void lcd(String path) 
		{ //throws SftpException{
			path=localAbsolutePath(path);
			if((new File(path)).isDirectory())
			{
				try
				{
					path=(new File(path)).getCanonicalPath();
				}
				catch(Exception e){}
				lcwd=path;
				return;
			}
			throw new SftpException(SSH_FX_NO_SUCH_FILE, "No such directory");
		}

		/*
		cd /tmp
		c->s REALPATH
		s->c NAME
		c->s STAT
		s->c ATTR
		*/
		public void cd(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);
      
				Vector v=glob_remote(path);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				path=(String)(v.elementAt(0));
				sendREALPATH(path.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=101 && type!=104)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				int i;
				if(type==101)
				{
					i=buf.getInt();
					throwStatusError(buf, i);
				}
				i=buf.getInt();
				byte[] str=buf.getString();
				if(str!=null && str[0]!='/')
				{
					str=(cwd+"/"+new String(str)).getBytes();
				}
				str=buf.getString();         // logname
				i=buf.getInt();              // attrs

				String newpwd=new String(str);
				SftpATTRS attr=_stat(newpwd);
				if((attr.getFlags()&SftpATTRS.SSH_FILEXFER_ATTR_PERMISSIONS)==0)
				{
					throw new SftpException(SSH_FX_FAILURE, 
					                        "Can't change directory: "+path);
				}
				if(!attr.isDir())
				{
					throw new SftpException(SSH_FX_FAILURE, 
					                        "Can't change directory: "+path);
				}
				cwd=newpwd;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		/*
		put foo
		c->s OPEN
		s->c HANDLE
		c->s WRITE
		s->c STATUS
		c->s CLOSE
		s->c STATUS
		*/
		public void put(String src, String dst) 
		{ //throws SftpException{
			put(src, dst, null, OVERWRITE);
		}
		public void put(String src, String dst, int mode) 
		{ //throws SftpException{
			put(src, dst, null, mode);
		}
		public void put(String src, String dst,
			SftpProgressMonitor monitor) 
		{ //throws SftpException{
			put(src, dst, monitor, OVERWRITE);
		}
		public void put(String src, String dst,
			SftpProgressMonitor monitor, int mode)
		{
			//throws SftpException{
			src=localAbsolutePath(src);
			dst=remoteAbsolutePath(dst);

			//System.err.println("src: "+src+", "+dst);
			try
			{
				Vector v=glob_remote(dst);
				int vsize=v.size();
				if(vsize!=1)
				{
					if(vsize==0)
					{
						if(isPattern(dst))
							throw new SftpException(SSH_FX_FAILURE, dst);
						else
							dst=Util.unquote(dst);
					}
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				else
				{
					dst=(String)(v.elementAt(0));
				}

				//System.err.println("dst: "+dst);

				bool _isRemoteDir=isRemoteDir(dst);

				v=glob_local(src);
				//System.err.println("glob_local: "+v+" dst="+dst);
				vsize=v.size();

				StringBuffer dstsb=null;
				if(_isRemoteDir)
				{
					if(!dst.endsWith("/"))
					{
						dst+="/";
					}
					dstsb=new StringBuffer(dst);
				}
				else if(vsize>1)
				{
					throw new SftpException(SSH_FX_FAILURE, "Copying multiple files, but destination is missing or a file.");
				}

				for(int j=0; j<vsize; j++)
				{
					String _src=(String)(v.elementAt(j));
					String _dst=null;
					if(_isRemoteDir)
					{
						int i=_src.lastIndexOf(file_separatorc);
						if(i==-1) dstsb.append(_src);
						else dstsb.append(_src.substring(i + 1));
						_dst=dstsb.toString();
						dstsb.delete(dst.length(), _dst.length());
					}
					else
					{
						_dst=dst;
					}
					//System.err.println("_dst "+_dst);

					long size_of_dst=0;
					if(mode==RESUME)
					{
						try
						{
							SftpATTRS attr=_stat(_dst);
							size_of_dst=attr.getSize();
						}
						catch(Exception eee)
						{
							//System.err.println(eee);
						}
						long size_of_src=new File(_src).length();
						if(size_of_src<size_of_dst)
						{
							throw new SftpException(SSH_FX_FAILURE, "failed to resume for "+_dst);
						}
						if(size_of_src==size_of_dst)
						{
							return;
						}
					}

					if(monitor!=null)
					{
						monitor.init(SftpProgressMonitor.PUT, _src, _dst,
						             (new File(_src)).length());
						if(mode==RESUME)
						{
							monitor.count(size_of_dst);
						}
					}
					FileInputStream fis=null;
					try
					{
						fis=new FileInputStream(_src);
						_put(fis, _dst, monitor, mode);
					}
					finally
					{
						if(fis!=null) 
						{
							//	    try{
							fis.close();
							//	    }catch(Exception ee){};
						}
					}
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, e.toString());
			}
		}
		public void put(InputStream src, String dst) 
		{ //throws SftpException{
			put(src, dst, null, OVERWRITE);
		}
		public void put(InputStream src, String dst, int mode) 
		{ //throws SftpException{
			put(src, dst, null, mode);
		}
		public void put(InputStream src, String dst,
			SftpProgressMonitor monitor) 
		{ //throws SftpException{
			put(src, dst, monitor, OVERWRITE);
		}
		public void put(InputStream src, String dst,
			SftpProgressMonitor monitor, int mode) 
		{ //throws SftpException{
    try{
      dst=remoteAbsolutePath(dst);
      Vector v=glob_remote(dst);
      int vsize=v.size();
      if(vsize!=1){
        if(vsize==0){
          if(isPattern(dst))
            throw new SftpException(SSH_FX_FAILURE, dst);
          else
            dst=Util.unquote(dst);
        }
        throw new SftpException(SSH_FX_FAILURE, v.toString());
      }
      else{
        dst=(String)(v.elementAt(0));
      }
      if(isRemoteDir(dst)){
        throw new SftpException(SSH_FX_FAILURE, dst+" is a directory");
      }
      _put(src, dst, monitor, mode);
    }
    catch(Exception e){
      if(e is SftpException) throw (SftpException)e;
      throw new SftpException(SSH_FX_FAILURE, e.toString());
    }
		}

		private void _put(InputStream src, String dst, 
			SftpProgressMonitor monitor, int mode)
		{
			try
			{
				long skip=0;
				if(mode==RESUME || mode==APPEND)
				{
					try
					{
						SftpATTRS attr=_stat(dst);
						skip=attr.getSize();
					}
					catch(Exception eee)
					{
						//System.err.println(eee);
					}
				}
				if(mode==RESUME && skip>0)
				{
					long skipped=src.skip(skip);
					if(skipped<skip)
					{
						throw new SftpException(SSH_FX_FAILURE, "failed to resume for "+dst);
					}
				}
				if(mode==OVERWRITE){ sendOPENW(dst.getBytes()); }
				else{ sendOPENA(dst.getBytes()); }

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
				{
					throw new SftpException(SSH_FX_FAILURE, "invalid type="+type);
				}
				if(type==SSH_FXP_STATUS)
				{
					int i=buf.getInt();
					throwStatusError(buf, i);
				}
				byte[] handle=buf.getString();         // filename
				byte[] data=null;

				bool dontcopy=true;

				if(!dontcopy)
				{
					data=new byte[buf.buffer.Length
					              -(5+13+21+handle.Length
					                +32 +20 // padding and mac
					               )
						];
				}

				long offset=0;
				if(mode==RESUME || mode==APPEND)
				{
					offset+=skip;
				}

				int startid=seq;
				int _ackid=seq;
				int ackcount=0;
				while(true)
				{
					int nread=0;
					int s=0;
					int datalen=0;
					int count=0;

					if(!dontcopy)
					{
						datalen=data.Length-s;
					}
					else
					{
						data=buf.buffer;
						s=5+13+21+handle.Length;
						datalen=buf.buffer.Length -s
						        -32 -20; // padding and mac
					}

					do
					{
						nread=src.read(data, s, datalen);
						if(nread>0)
						{
							s+=nread;
							datalen-=nread;
							count+=nread;
						}
					}
					while(datalen>0 && nread>0); 
					if(count<=0)break;

					int _i=count;
					while(_i>0)
					{
						_i-=sendWRITE(handle, offset, data, 0, _i);
						if((seq-1)==startid ||
						   io.ins.available()>=1024)
						{
							while(io.ins.available()>0)
							{
								if(checkStatus(ackid, _header))
								{
									_ackid=ackid[0];
									if(startid>_ackid || _ackid>seq-1)
									{
										if(_ackid==seq)
										{
											java.System.err.println("ack error: startid="+startid+" seq="+seq+" _ackid="+_ackid);
										} 
										else
										{
											//throw new SftpException(SSH_FX_FAILURE, "ack error:");
											throw new SftpException(SSH_FX_FAILURE, "ack error: startid="+startid+" seq="+seq+" _ackid="+_ackid);
										}
									}
									ackcount++;
								}
								else
								{
									break;
								}
							}
						}
					}
					offset+=count;
					if(monitor!=null && !monitor.count(count))
					{
						break;
					}
				}
				int _ackcount=seq-startid;
				while(_ackcount>ackcount)
				{
					if(!checkStatus(null, _header))
					{
						break;
					}
					ackcount++;
				}
				if(monitor!=null)monitor.end();
				_sendCLOSE(handle, _header);
				//System.err.println("start end "+startid+" "+endid);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, e.toString());
			}
		}

		private SftpATTRS _stat(String path)
		{
			try
			{
				sendSTAT(path.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_ATTRS)
				{
					if(type==SSH_FXP_STATUS)
					{
						int i=buf.getInt();
						throwStatusError(buf, i);
					}
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				SftpATTRS attr=SftpATTRS.getATTR(buf);
				return attr;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
			//return null;
		}

/**/

		public OutputStream put(String dst) 
		{ //throws SftpException{
			return put(dst, (SftpProgressMonitor)null, OVERWRITE);
		}
		public OutputStream put(String dst,  int mode) 
		{ //throws SftpException{
			return put(dst, (SftpProgressMonitor)null, mode);
		}
		public OutputStream put(String dst,  SftpProgressMonitor monitor,  int mode) 
		{ //throws SftpException{
			return put(dst, monitor, mode, 0);
		}
		public OutputStream put(String dst, SftpProgressMonitor monitor, int mode, long offset) 
		{
			dst=remoteAbsolutePath(dst);
			try
			{
				Vector v=glob_remote(dst);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				dst=(String)(v.elementAt(0));
				if(isRemoteDir(dst))
				{
					throw new SftpException(SSH_FX_FAILURE, dst+" is a directory");
				}

				long skip=0;
				if(mode==RESUME || mode==APPEND)
				{
					try
					{
						SftpATTRS attr=stat(dst);
						skip=attr.getSize();
					}
					catch(Exception eee)
					{
						//System.out.println(eee);
					}
				}

				if(mode==OVERWRITE){ sendOPENW(dst.getBytes()); }
				else{ sendOPENA(dst.getBytes()); }

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				
				buf.rewind();
				fill(buf.buffer, 0, length);
				
				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				if(type==SSH_FXP_STATUS)
				{
					int i=buf.getInt();
					throwStatusError(buf, i);
				}
				
				byte[] handle=buf.getString();         // filename
				
				//long offset=0;
				if(mode==RESUME || mode==APPEND)
				{
					offset+=skip;
				}

				long[] _offset=new long[1];
				_offset[0]=offset;
				OutputStream outs = new OutputStreamPut(this,handle,_offset,monitor);
				//  private bool init=true;
				//  private int[] ackid=new int[1];
				//  private int startid=0;
				//  private int _ackid=0;
				//  private int ackcount=0;

				//  public void write(byte[] d, int s, int len) { //throws java.io.IOException{

				//    if(init){
				//      startid=count;
				//      _ackid=count;
				//      init=false;
				//    }

				//    try{
				//      int _len=len;
				//      while(_len>0){
				//        _len-=sendWRITE(handle, _offset[0], d, s, _len);

				//        if((count-1)==startid ||
				//           io.ins.available()>=1024){
				//          while(io.ins.available()>0){
				//            if(checkStatus(ackid)){
				//              _ackid=ackid[0];
				//              if(startid>_ackid || _ackid>count-1){
				//                throw new SftpException(SSH_FX_FAILURE, "");
				//              }
				//              ackcount++;
				//            }
				//            else{
				//              break;
				//            }
				//          }
				//        }

				//      }
				//      _offset[0]+=len;
				//      if(monitor!=null && !monitor.count(len)){
				//        throw new IOException("canceled");
				//  }
				//    }
				//    catch(IOException e){ throw e; }
				//    catch(Exception e){ throw new IOException(e.toString());  }
				//  }
				//  byte[] _data=new byte[1];
				//  public void write(int foo) { //throws java.io.IOException{
				//    _data[0]=(byte)foo;
				//    write(_data, 0, 1);
				//  }
				//  public void close() { //throws java.io.IOException{

				//    try{
				//      int _ackcount=count-startid;
				//      while(_ackcount>ackcount){
				//        if(!checkStatus(null)){
				//          break;
				//        }
				//        ackcount++;
				//      }
				//    }
				//    catch(SftpException e){
				//      throw new IOException(e.toString());
				//    }

				//    if(monitor!=null)monitor.end();
				//    try{ _sendCLOSE(handle); }
				//    catch(IOException e){ throw e; }
				//    catch(Exception e){
				//      throw new IOException(e.toString());
				//    }
				//  }
				//};
				return outs;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		/**/
		public void get(String src, String dst) 
		{ //throws SftpException{
			get(src, dst, null, OVERWRITE);
		}
		public void get(String src, String dst,
			SftpProgressMonitor monitor) 
		{ //throws SftpException{
			get(src, dst, monitor, OVERWRITE);
		}
		public void get(String src, String dst,
			SftpProgressMonitor monitor, int mode)
		{
			//throws SftpException{
			src=remoteAbsolutePath(src);
			dst=localAbsolutePath(dst);
			try
			{
				Vector v=glob_remote(src);
				int vsize=v.size();
				if(vsize==0)
				{
					throw new SftpException(SSH_FX_NO_SUCH_FILE, "No such file");
				}

				File dstFile=new File(dst);
				bool isDstDir=dstFile.isDirectory();
				StringBuffer dstsb=null;
				if(isDstDir)
				{
					if(!dst.endsWith(file_separator))
					{
						dst+=file_separator;
					}
					dstsb=new StringBuffer(dst);
				}
				else if(vsize>1)
				{
					throw new SftpException(SSH_FX_FAILURE, "Copying multiple files, but destination is missing or a file.");
				}

				for(int j=0; j<vsize; j++)
				{
					String _src=(String)(v.elementAt(j));

					SftpATTRS attr=_stat(_src);
					if(attr.isDir())
					{
						throw new SftpException(SSH_FX_FAILURE, "not supported to get directory "+_src);
					} 

					String _dst=null;
					if(isDstDir)
					{
						int i=_src.lastIndexOf('/');
						if(i==-1) dstsb.append(_src);
						else dstsb.append(_src.substring(i + 1));
						_dst=dstsb.toString();
						dstsb.delete(dst.length(), _dst.length());
					}
					else
					{
						_dst=dst;
					}

					if(mode==RESUME)
					{
						long size_of_src=attr.getSize();
						long size_of_dst=new File(_dst).length();
						if(size_of_dst>size_of_src)
						{
							throw new SftpException(SSH_FX_FAILURE, "failed to resume for "+_dst);
						}
						if(size_of_dst==size_of_src)
						{
							return;
						}
					}

					if(monitor!=null)
					{
						monitor.init(SftpProgressMonitor.GET, _src, _dst, attr.getSize());
						if(mode==RESUME)
						{
							monitor.count(new File(_dst).length());
						}
					}
					FileOutputStream fos=null;
					if(mode==OVERWRITE)
					{
						fos=new FileOutputStream(_dst);
					}
					else
					{
						fos=new FileOutputStream(_dst, true); // append
					}

					//System.err.println("_get: "+_src+", "+_dst);
					_get(_src, fos, monitor, mode, new File(_dst).length());
					fos.close();
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void get(String src, OutputStream dst) 
		{ //throws SftpException{
			get(src, dst, null, OVERWRITE, 0);
		}
		public void get(String src, OutputStream dst,
			SftpProgressMonitor monitor) 
		{ //throws SftpException{
			get(src, dst, monitor, OVERWRITE, 0);
		}
		public void get(String src, OutputStream dst,
			SftpProgressMonitor monitor, int mode, long skip)
		{
			//throws SftpException{
			//try
			//{
				src=remoteAbsolutePath(src);
				Vector v=glob_remote(src);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				src=(String)(v.elementAt(0));

				if(monitor!=null)
				{
					SftpATTRS attr=_stat(src);
					monitor.init(SftpProgressMonitor.GET, src, "??", attr.getSize());
					if(mode==RESUME)
					{
						monitor.count(skip);
					}
				}
				_get(src, dst, monitor, mode, skip);
			//}
			//catch(Exception e)
			//{
			//	if(e is SftpException) throw (SftpException)e;
			//	throw new SftpException(SSH_FX_FAILURE, "");
			//}
		}
		
		///tamir: updated to jcsh-0.1.30
		private void _get(String src, OutputStream dst,
			SftpProgressMonitor monitor, int mode, long skip) 
		{ //throws SftpException{
			//System.out.println("_get: "+src+", "+dst);
			//try
			//{
				sendOPENR(src.getBytes());
				
				
				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				
				buf.rewind();
				
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
				{
					//System.Console.WriteLine("Type is "+type);
					throw new SftpException(SSH_FX_FAILURE, "Type is "+type);
				}

				if(type==SSH_FXP_STATUS)
				{
					int i=buf.getInt();
					throwStatusError(buf, i);
				}
				
				byte[] handle=buf.getString();         // filename

				long offset=0;
				if(mode==RESUME)
				{
					offset+=skip;
				}

				int request_len=0;
			loop:
				while(true)
				{

					request_len=buf.buffer.Length-13;
					if(server_version==0){ request_len=1024; }
					sendREAD(handle, offset, request_len);
					
					_header=header(buf, _header);
					length=_header.length;
					type=_header.type;
					
					int i;
					if(type==SSH_FXP_STATUS)
					{
						buf.rewind();
						fill(buf.buffer, 0, length);
						i=buf.getInt();   
						if(i==SSH_FX_EOF)
						{
							goto BREAK;
						}
						throwStatusError(buf, i);
					}
					
					if(type!=SSH_FXP_DATA)
					{
						goto BREAK;
					}

					buf.rewind();
					fill(buf.buffer, 0, 4); length-=4;
					i=buf.getInt();   // length of data 
					int foo=i;
					while(foo>0)
					{
						int bar=foo;
						if(bar>buf.buffer.Length)
						{
							bar=buf.buffer.Length;
						}
						i=io.ins.read(buf.buffer, 0, bar);
						if(i<0)
						{
							goto BREAK;
						}
						int data_len=i;
						dst.write(buf.buffer, 0, data_len);

						offset+=data_len;
						foo-=data_len;

						if(monitor!=null)
						{
							if(!monitor.count(data_len))
							{
								while(foo>0)
								{
									i=io.ins.read(buf.buffer, 
										0, 
										(buf.buffer.Length<foo?buf.buffer.Length:foo));
									if(i<=0) break;
									foo-=i;
								}
								goto BREAK;
							}
						}
					}
					//System.out.println("length: "+length);  // length should be 0
				}
			BREAK:
				dst.flush();

				if(monitor!=null)monitor.end();
				_sendCLOSE(handle, _header);
			//}
			//catch(Exception e)
			//{
			//	//System.Console.WriteLine(e);
			//	if(e is SftpException) throw (SftpException)e;
			//	throw new SftpException(SSH_FX_FAILURE, "");
			//}
		}

		public InputStream get(String src) 
		{ //throws SftpException{
			return get(src, null, OVERWRITE);
		}
		public InputStream get(String src, SftpProgressMonitor monitor) 
		{ //throws SftpException{
			return get(src, monitor, OVERWRITE);
		}
		public InputStream get(String src, int mode) 
		{ //throws SftpException{
			return get(src, null, mode);
		}
		public InputStream get(String src,  SftpProgressMonitor monitor,  int mode) 
		{ //throws SftpException{
			if(mode==RESUME)
			{
				throw new SftpException(SSH_FX_FAILURE, "faile to resume from "+src);
			}
			src=remoteAbsolutePath(src);
			try
			{
				Vector v=glob_remote(src);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				src=(String)(v.elementAt(0));

				SftpATTRS attr=_stat(src);
				if(monitor!=null)
				{
					monitor.init(SftpProgressMonitor.GET, src, "??", attr.getSize());
				}

				sendOPENR(src.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				if(type==SSH_FXP_STATUS)
				{
					int i=buf.getInt();
					throwStatusError(buf, i);
				}

				byte[] handle=buf.getString();         // filename

				java.io.InputStream ins=new InputStreamGet(this, handle, monitor);
				//  long offset=0;
				//  bool closed=false;
				//  int rest_length=0;
				//  byte[] _data=new byte[1];
				//  public int read() { //throws java.io.IOException{
				//    int i=read(_data, 0, 1);
				//    if (i==-1) { return -1; }
				//    else {
				//      return _data[0]&0xff;
				//    }
				//  }
				//  public int read(byte[] d) { //throws java.io.IOException{
				//    return read(d, 0, d.Length);
				//  }
				//  public int read(byte[] d, int s, int len) { //throws java.io.IOException{
				//    if(d==null){throw new NullPointerException();}
				//    if(s<0 || len <0 || s+len>d.Length){
				//      throw new IndexOutOfBoundsException();
				//    }
				//    if(len==0){ return 0; }

				//    if(rest_length>0){
				//      int foo=rest_length;
				//      if(foo>len) foo=len;
				//      int i=io.ins.read(d, s, foo);
				//      if(i<0){
				//        throw new IOException("error");
				//      }
				//      rest_length-=i;
				//      return i;
				//    }

				//    if(buf.buffer.Length-13<len){
				//      len=buf.buffer.Length-13;
				//    }
				//    if(server_version==0 && len>1024){
				//      len=1024;
				//    }

				//    try{sendREAD(handle, offset, len);}
				//    catch(Exception e){ throw new IOException("error"); }

				//    buf.rewind();
				//    int i=io.ins.read(buf.buffer, 0, 13);  // 4 + 1 + 4 + 4
				//    if(i!=13){
				//      throw new IOException("error");
				//    }

				//    rest_length=buf.getInt();
				//    int type=buf.getByte();
				//    rest_length--;
				//    buf.getInt();
				//    rest_length-=4;
				//    if(type!=SSH_FXP_STATUS && type!=SSH_FXP_DATA){
				//      throw new IOException("error");
				//    }
				//    if(type==SSH_FXP_STATUS){
				//      i=buf.getInt();
				//      rest_length-=4;
				//      io.ins.read(buf.buffer, 13, rest_length);
				//      rest_length=0;
				//      if(i==SSH_FX_EOF){
				//        close();
				//        return -1;
				//      }
				//      //throwStatusError(buf, i);
				//      throw new IOException("error");
				//    }

				//    i=buf.getInt();
				//    rest_length-=4;
				//    offset+=rest_length;
				//    int foo=i;
				//    if(foo>0){
				//      int bar=rest_length;
				//      if(bar>len){
				//        bar=len;
				//      }
				//      i=io.ins.read(d, s, bar);
				//      if(i<0){
				//        return -1;
				//      }
				//      rest_length-=i;

				//      if(monitor!=null){
				//        if(!monitor.count(i)){
				//          return -1;
				//        }
				//      }
				//      return i;
				//    }
				//    return 0; // ??
				//  }
				//  public void close() { //throws IOException{
				//    if(closed)return;
				//    closed=true;
				//    /*
				//    while(rest_length>0){
				//      int foo=rest_length;
				//      if(foo>buf.buffer.Length){
				//        foo=buf.buffer.Length;
				//      }
				//      io.ins.read(buf.buffer, 0, foo);
				//      rest_length-=foo;
				//    }
				//    */
				//    if(monitor!=null)monitor.end();
				//    try{_sendCLOSE(handle);}
				//    catch(Exception e){throw new IOException("error");}
				//  }
				//};
				return ins;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		public java.util.Vector ls(String path) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				String dir=path;
				byte[] pattern=null;
				SftpATTRS attr=null;
				if(isPattern(dir) || 
					((attr=stat(dir))!=null && !attr.isDir()))
				{
					int foo=path.lastIndexOf('/');
					dir=path.substring(0, ((foo==0)?1:foo));
					pattern=path.substring(foo+1).getBytes();
				}

				sendOPENDIR(dir.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				if(type==SSH_FXP_STATUS)
				{
					int i=buf.getInt();
					throwStatusError(buf, i);
				}

				byte[] handle=buf.getString();         // filename

				java.util.Vector v=new java.util.Vector();
				while(true)
				{
					sendREADDIR(handle);

					_header=header(buf, _header);
					length=_header.length;
					type=_header.type;
					if(type!=SSH_FXP_STATUS && type!=SSH_FXP_NAME)
					{
						throw new SftpException(SSH_FX_FAILURE, "");
					}
					if(type==SSH_FXP_STATUS)
					{ 
						buf.rewind();
						fill(buf.buffer, 0, length);
						int i=buf.getInt();
						if(i==SSH_FX_EOF)
							break;
						throwStatusError(buf, i);
					}

					buf.rewind();
					fill(buf.buffer, 0, 4); length-=4;
					int count=buf.getInt();

					byte[] str;
					int flags;

					buf.reset();
					while(count>0)
					{
						if(length>0)
						{
							buf.shift();
							int j=(buf.buffer.Length>(buf.index+length)) ? length : (buf.buffer.Length-buf.index);
							int i=fill(buf.buffer, buf.index, j);
							buf.index+=i;
							length-=i;
						}
						byte[] filename=buf.getString();
						str=buf.getString();
						String longname=new String(str);

						SftpATTRS attrs=SftpATTRS.getATTR(buf);
						if(pattern==null || Util.glob(pattern, filename))
						{
							v.addElement(new LsEntry(new String(filename), longname, attrs));
						}

						count--; 
					}
				}
				_sendCLOSE(handle, _header);
				return v;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		
		public String readlink(String path)
		{
			// throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);
				Vector v=glob_remote(path);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				path=(String)(v.elementAt(0));

				sendREADLINK(path.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_NAME)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				int i;
				if(type==SSH_FXP_NAME)
				{
					int count=buf.getInt();       // count
					byte[] filename=null;
					byte[] longname=null;
					for(i=0; i<count; i++)
					{
						filename=buf.getString();
						longname=buf.getString();
						SftpATTRS.getATTR(buf);
					}
					return new String(filename);
				}

				i=buf.getInt();
				throwStatusError(buf, i);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
			return null;
		}


		public void symlink(String oldpath, String newpath)
		{
			//throws SftpException{
			if(server_version<3)
			{
				throw new SftpException(SSH_FX_FAILURE, 
				                        "The remote sshd is too old to support symlink operation.");
			}

			try
			{
				oldpath=remoteAbsolutePath(oldpath);
				newpath=remoteAbsolutePath(newpath);

				Vector v=glob_remote(oldpath);
				int vsize=v.size();
				if(vsize!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				oldpath=(String)(v.elementAt(0));

				if(isPattern(newpath))
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}

				newpath=Util.unquote(newpath);

				sendSYMLINK(oldpath.getBytes(), newpath.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}

				int i=buf.getInt();
				if(i==SSH_FX_OK) return;
				throwStatusError(buf, i);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void rename(String oldpath, String newpath)
		{
			//throws SftpException{
			if(server_version<2)
			{
				throw new SftpException(SSH_FX_FAILURE, 
				                        "The remote sshd is too old to support rename operation.");
			}
			try
			{
				oldpath=remoteAbsolutePath(oldpath);
				newpath=remoteAbsolutePath(newpath);

				Vector v=glob_remote(oldpath);
				int vsize=v.size();
				if(vsize!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				oldpath=(String)(v.elementAt(0));

				v=glob_remote(newpath);
				vsize=v.size();
				if(vsize>=2)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				if(vsize==1)
				{
					newpath=(String)(v.elementAt(0));
				}
				else
				{  // vsize==0
					if(isPattern(newpath))
						throw new SftpException(SSH_FX_FAILURE, newpath);
					newpath=Util.unquote(newpath);
				}

				sendRENAME(oldpath.getBytes(), newpath.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}

				int i=buf.getInt();
				if(i==SSH_FX_OK) return;
				throwStatusError(buf, i);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void rm(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);
				Vector v=glob_remote(path);
				int vsize=v.size();
				Header _header=new Header();

				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));
					sendREMOVE(path.getBytes());

					_header=header(buf, _header);
					int length=_header.length;
					int type=_header.type;
					buf.rewind();
					fill(buf.buffer, 0, length);

					if(type!=SSH_FXP_STATUS)
					{
						throw new SftpException(SSH_FX_FAILURE, "");
					}
					int i=buf.getInt();
					if(i!=SSH_FX_OK)
					{
						throwStatusError(buf, i);
					}
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		private bool isRemoteDir(String path)
		{
			try
			{
				sendSTAT(path.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_ATTRS)
				{
					return false; 
				}
				SftpATTRS attr=SftpATTRS.getATTR(buf);
				return attr.isDir();
			}
			catch(Exception e){}
			return false;
		}
		/*
		bool isRemoteDir(String path) { //throws SftpException{
		SftpATTRS attr=stat(path);
		return attr.isDir();
		}
		*/
		public void chgrp(int gid, String path) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));
					
					SftpATTRS attr=_stat(path);
					
					attr.setFLAGS(0);
					attr.setUIDGID(attr.uid, gid);
					_setStat(path, attr);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void chown(int uid, String path) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));

					SftpATTRS attr=_stat(path);
					
					attr.setFLAGS(0);
					attr.setUIDGID(uid, attr.gid);
					_setStat(path, attr);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void chmod(int permissions, String path) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));

					SftpATTRS attr=_stat(path);
					
					attr.setFLAGS(0);
					attr.setPERMISSIONS(permissions);
					_setStat(path, attr);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void setMtime(String path, int mtime) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));
					
					SftpATTRS attr=_stat(path);
					
					attr.setFLAGS(0);
					attr.setACMODTIME(attr.getATime(), mtime);
					_setStat(path, attr);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		public void rmdir(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				Header _header=new Header();

				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));
					sendRMDIR(path.getBytes());

					_header=header(buf, _header);
					int length=_header.length;
					int type=_header.type;
					buf.rewind();
					fill(buf.buffer, 0, length);

					if(type!=SSH_FXP_STATUS)
					{
						throw new SftpException(SSH_FX_FAILURE, "");
					}

					int i=buf.getInt();
					if(i!=SSH_FX_OK)
					{
						throwStatusError(buf, i);
					}
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		public void mkdir(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				sendMKDIR(path.getBytes(), null);

				Header _header=new Header();      
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}

				int i=buf.getInt();
				if(i==SSH_FX_OK) return;
				throwStatusError(buf, i);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		public SftpATTRS stat(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				path=(String)(v.elementAt(0));
				return _stat(path);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
			//return null;
		}
		public SftpATTRS lstat(String path)
		{
			//throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				if(v.size()!=1)
				{
					throw new SftpException(SSH_FX_FAILURE, v.toString());
				}
				path=(String)(v.elementAt(0));

				return _lstat(path);
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		
		private SftpATTRS _lstat(String path)
		{
			//throws SftpException{
			try
			{
				sendLSTAT(path.getBytes());

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_ATTRS)
				{
					if(type==SSH_FXP_STATUS)
					{
						int i=buf.getInt();
						throwStatusError(buf, i);
					}
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				SftpATTRS attr=SftpATTRS.getATTR(buf);
				return attr;
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
  

		public void setStat(String path, SftpATTRS attr) 
		{ //throws SftpException{
			try
			{
				path=remoteAbsolutePath(path);

				Vector v=glob_remote(path);
				int vsize=v.size();
				for(int j=0; j<vsize; j++)
				{
					path=(String)(v.elementAt(j));
					_setStat(path, attr);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}
		private void _setStat(String path, SftpATTRS attr)
		{
			//throws SftpException{
			try
			{
				sendSETSTAT(path.getBytes(), attr);

				Header _header=new Header();
				_header=header(buf, _header);
				int length=_header.length;
				int type=_header.type;
				buf.rewind();
				fill(buf.buffer, 0, length);

				if(type!=SSH_FXP_STATUS)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				int i=buf.getInt();
				if(i!=SSH_FX_OK)
				{
					throwStatusError(buf, i);
				}
			}
			catch(Exception e)
			{
				if(e is SftpException) throw (SftpException)e;
				throw new SftpException(SSH_FX_FAILURE, "");
			}
		}

		public String pwd(){ return cwd; }
		public String lpwd(){ return lcwd; }
		public String version(){ return _version; }
		public String getHome(){ return home; }

		private void read(byte[] buf, int s, int l) 
		{ //throws IOException, SftpException{
			int i=0;
			while(l>0)
			{
				i=io.ins.read(buf, s, l);
				if(i<=0)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				s+=i;
				l-=i;
			}
		}
		internal bool checkStatus(int[] ackid, Header _header ) 
		{ //throws IOException, SftpException{
			_header=header(buf, _header);
			int length=_header.length;
			int type=_header.type;
			if(ackid!=null)
				ackid[0]=_header.rid;
			buf.rewind();
			fill(buf.buffer, 0, length);

			if(type!=SSH_FXP_STATUS)
			{ 
				throw new SftpException(SSH_FX_FAILURE, "");
			}
			int i=buf.getInt();
			if(i!=SSH_FX_OK)
			{
				throwStatusError(buf, i);
			}
			return true;
		}
		
		internal bool _sendCLOSE(byte[] handle, Header header)
			//throws Exception
		{
			sendCLOSE(handle);
			return checkStatus(null, header);
		}

		private void sendINIT() 
		{ //throws Exception{
			packet.reset();
			putHEAD(SSH_FXP_INIT, 5);
			buf.putInt(3);                // version 3
			session.write(packet, this, 5+4);
		}

		private void sendREALPATH(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_REALPATH, path);
		}
		private void sendSTAT(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_STAT, path);
		}
		private void sendLSTAT(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_LSTAT, path);
		}
		private void sendFSTAT(byte[] handle) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_FSTAT, handle);
		}
		private void sendSETSTAT(byte[] path, SftpATTRS attr) 
		{ //throws Exception{
			packet.reset();
			putHEAD(SSH_FXP_SETSTAT, 9+path.Length+attr.Length());
			buf.putInt(seq++);
			buf.putString(path);             // path
			attr.dump(buf);
			session.write(packet, this, 9+path.Length+attr.Length()+4);
		}
		private void sendREMOVE(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_REMOVE, path);
		}
		private void sendMKDIR(byte[] path, SftpATTRS attr) 
		{ //throws Exception{
			packet.reset();
			putHEAD(SSH_FXP_MKDIR, 9+path.Length+(attr!=null?attr.Length():4));
			buf.putInt(seq++);
			buf.putString(path);             // path
			if(attr!=null) attr.dump(buf);
			else buf.putInt(0);
			session.write(packet, this, 9+path.Length+(attr!=null?attr.Length():4)+4);
		}
		private void sendRMDIR(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_RMDIR, path);
		}
		private void sendSYMLINK(byte[] p1, byte[] p2) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_SYMLINK, p1, p2);
		}
		private void sendREADLINK(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_READLINK, path);
		}
		private void sendOPENDIR(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_OPENDIR, path);
		}
		private void sendREADDIR(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_READDIR, path);
		}
		private void sendRENAME(byte[] p1, byte[] p2) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_RENAME, p1, p2);
		}
		private void sendCLOSE(byte[] path) 
		{ //throws Exception{
			sendPacketPath(SSH_FXP_CLOSE, path);
		}
		private void sendOPENR(byte[] path) 
		{ //throws Exception{
			sendOPEN(path, SSH_FXF_READ);
		}
		private void sendOPENW(byte[] path) 
		{ //throws Exception{
			sendOPEN(path, SSH_FXF_WRITE|SSH_FXF_CREAT|SSH_FXF_TRUNC);
		}
		private void sendOPENA(byte[] path) 
		{ //throws Exception{
			sendOPEN(path, SSH_FXF_WRITE|/*SSH_FXF_APPEND|*/SSH_FXF_CREAT);
		}
		private void sendOPEN(byte[] path, int mode) 
		{ //throws Exception{
			packet.reset();
			putHEAD(SSH_FXP_OPEN, 17+path.Length);
			buf.putInt(seq++);
			buf.putString(path);
			buf.putInt(mode);
			buf.putInt(0);           // attrs
			session.write(packet, this, 17+path.Length+4);
		}
		private void sendPacketPath(byte fxp, byte[] path) 
		{ //throws Exception{
			packet.reset();
			putHEAD(fxp, 9+path.Length);
			buf.putInt(seq++);
			buf.putString(path);             // path
			session.write(packet, this, 9+path.Length+4);
		}
		private void sendPacketPath(byte fxp, byte[] p1, byte[] p2) 
		{ //throws Exception{
			packet.reset();
			putHEAD(fxp, 13+p1.Length+p2.Length);
			buf.putInt(seq++);
			buf.putString(p1);
			buf.putString(p2);
			session.write(packet, this, 13+p1.Length+p2.Length+4);
		}

		internal int sendWRITE(byte[] handle, long offset,
			byte[] data, int start, int length) 
		{ //throws Exception{
			int _length=length;
			packet.reset();
			if(buf.buffer.Length<buf.index+13+21+handle.Length+length
				+32 +20  // padding and mac
				)
			{
				_length=buf.buffer.Length-(buf.index+13+21+handle.Length
					+32 +20  // padding and mac
					);
				//System.err.println("_length="+_length+" length="+length);
			}
			putHEAD(SSH_FXP_WRITE, 21+handle.Length+_length);       // 14
			buf.putInt(seq++);                                    //  4
			buf.putString(handle);                                  //  4+handle.length
			buf.putLong(offset);                                    //  8
			if(buf.buffer!=data)
			{
				buf.putString(data, start, _length);                    //  4+_length
			}
			else
			{
				buf.putInt(_length);
				buf.skip(_length);
			}
			session.write(packet, this, 21+handle.Length+_length+4);
			return _length;
		}

		private void sendREAD(byte[] handle, long offset, int length) 
		{ //throws Exception{
			packet.reset();
			putHEAD(SSH_FXP_READ, 21+handle.Length);
			buf.putInt(seq++);
			buf.putString(handle);
			buf.putLong(offset);
			buf.putInt(length);
			session.write(packet, this, 21+handle.Length+4);
		}

		private void putHEAD(byte type, int length) 
		{ //throws Exception{
			buf.putByte((byte)Session.SSH_MSG_CHANNEL_DATA);
			buf.putInt(recipient);
			buf.putInt(length+4);
			buf.putInt(length);
			buf.putByte(type);
		}
		private Vector glob_remote(String _path)
		{
			//throws Exception{
			//System.err.println("glob_remote: "+_path);
			Vector v=new Vector();
			byte[] path=_path.getBytes();
			if(!isPattern(path))
			{
				v.addElement(Util.unquote(_path)); return v;
			}
			int i=path.Length-1;
			while(i>=0){if(path[i]=='/')break;i--;}
			if(i<0){ v.addElement(Util.unquote(_path)); return v;}
			byte[] dir;
			if(i==0){dir=new byte[]{(byte)'/'};}
			else
			{ 
				dir=new byte[i];
				java.System.arraycopy(path, 0, dir, 0, i);
			}
			//System.err.println("dir: "+new String(dir));
			byte[] pattern=new byte[path.Length-i-1];
			java.System.arraycopy(path, i+1, pattern, 0, pattern.Length);
			//System.err.println("file: "+new String(pattern));

			sendOPENDIR(dir);

			Header _header=new Header();
			_header=header(buf, _header);
			int length=_header.length;
			int type=_header.type;
			buf.rewind();
			fill(buf.buffer, 0, length);

			if(type!=SSH_FXP_STATUS && type!=SSH_FXP_HANDLE)
			{
				throw new SftpException(SSH_FX_FAILURE, "");
			}
			if(type==SSH_FXP_STATUS)
			{
				i=buf.getInt();
				throwStatusError(buf, i);
			}

			byte[] handle=buf.getString();         // filename

			while(true)
			{
				sendREADDIR(handle);
				_header=header(buf, _header);
				length=_header.length;
				type=_header.type;

				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_NAME)
				{
					throw new SftpException(SSH_FX_FAILURE, "");
				}
				if(type==SSH_FXP_STATUS)
				{ 
					buf.rewind();
					fill(buf.buffer, 0, length);
					break;
				}

				buf.rewind();
				fill(buf.buffer, 0, 4); length-=4;
				int count=buf.getInt();

				byte[] str;
				int flags;

				buf.reset();
				while(count>0)
				{
					if(length>0)
					{
						buf.shift();
						int j=(buf.buffer.Length>(buf.index+length)) ? length : (buf.buffer.Length-buf.index);
						i=io.ins.read(buf.buffer, buf.index, j);
						if(i<=0)break;
						buf.index+=i;
						length-=i;
					}

					byte[] filename=buf.getString();
					//System.err.println("filename: "+new String(filename));
					str=buf.getString();
					SftpATTRS attrs=SftpATTRS.getATTR(buf);

					if(Util.glob(pattern, filename))
					{
						v.addElement(new String(dir)+"/"+new String(filename));
					}
					count--; 
				}
			}
			if(_sendCLOSE(handle, _header)) 
				return v;
			return null;
		}

		private Vector glob_local(String _path) 
		{ //throws Exception{
			//System.out.println("glob_local: "+_path);
			Vector v=new Vector();
			byte[] path=_path.getBytes();
			int i=path.Length-1;
			while(i>=0){if(path[i]=='*' || path[i]=='?')break;i--;}
			if(i<0){ v.addElement(_path); return v;}
			while(i>=0){if(path[i]==file_separatorc)break;i--;}
			if(i<0){ v.addElement(_path); return v;}
			byte[] dir;
			if(i==0){dir=new byte[]{(byte)file_separatorc};}
			else
			{
				dir=new byte[i];
				Tamir.SharpSsh.java.System.arraycopy(path, 0, dir, 0, i);
			}
			byte[] pattern=new byte[path.Length-i-1];
			Tamir.SharpSsh.java.System.arraycopy(path, i+1, pattern, 0, pattern.Length);
			//System.out.println("dir: "+new String(dir)+" pattern: "+new String(pattern));
			try
			{
				String[] children=(new File(new String(dir))).list();
				for(int j=0; j<children.Length; j++)
				{
					//System.out.println("children: "+children[j]);
					if(Util.glob(pattern, children[j].getBytes()))
					{
						v.addElement(new String(dir)+file_separator+children[j]);
					}
				}
			}
			catch(Exception e)
			{
			}
			return v;
		}

		private void throwStatusError(Buffer buf, int i) 
		{ //throws SftpException{
			if(server_version>=3)
			{
				byte[] str=buf.getString();
				//byte[] tag=buf.getString();
				throw new SftpException(i, new String(str));
			}
			else
			{
				throw new SftpException(i, "Failure");
			}
		}

		private static bool isLocalAbsolutePath(String path)
		{
			return (new File(path)).isAbsolute();
		}

		/*
		public void finalize() { //throws Throwable{
		base.finalize();
		}
		*/

		public override void disconnect()
		{
			//waitForRunningThreadFinish(10000);
			clearRunningThreads();
			base.disconnect();
		}
		private java.util.Vector threadList=null;
		[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
		protected void addRunningThread(Thread thread)
		{
			if(threadList==null)threadList=new java.util.Vector();
			threadList.add(thread);
		}
		[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
		protected void clearRunningThreads()
		{
			if(threadList==null)return;
			for(int t=0;t<threadList.size();t++)
			{
				Thread thread=(Thread)threadList.get(t);
				if(thread!=null)
					if(thread.isAlive())
						thread.interrupt();
			}
			threadList.clear();
		}
		private bool isPattern(String path)
		{
			return path.indexOf("*")!=-1 || path.indexOf("?")!=-1;
		}
		private bool isPattern(byte[] path)
		{
			int i=path.Length-1;
			while(i>=0)
			{
				if(path[i]=='*' || path[i]=='?')
				{
					if(i>0 && path[i-1]=='\\')
					{
						i--;
					}
					else
					{
						break;
					}
				}
				i--;
			}
			//System.err.println("isPattern: ["+(new String(path))+"] "+(!(i<0)));
			return !(i<0);
		}

		private int fill(byte[] buf, int s, int len) 
		{
			int i=0;
			int foo=s;
			while(len>0)
			{
				i=io.ins.read(buf, s, len);
				if(i<=0)
				{
					throw new System.IO.IOException("inputstream is closed");
					//return (s-foo)==0 ? i : s-foo;
				}
				s+=i;
				len-=i;
			}
			return s-foo;
		}
		
		//tamir: some functions from jsch-0.1.30
		private void skip(long foo)
		{
			while(foo>0)
			{
				long bar=io.ins.skip(foo);
				if(bar<=0) 
					break;
				foo-=bar;
			}
		}
	
		internal class Header
		{
			public int length;
			public int type;
			public int rid;
		}
		internal Header header(Buffer buf, Header header)
		{
			buf.rewind();
			int i=fill(buf.buffer, 0, 9);
			header.length=buf.getInt()-5;
			header.type=buf.getByte()&0xff;
			header.rid=buf.getInt();  
			return header;
		}

		private String remoteAbsolutePath(String path)
		{
			if(path.charAt(0)=='/') return path;
			if(cwd.endsWith("/")) return cwd+path;
			return cwd+"/"+path;
		}

		private String localAbsolutePath(String path)
		{
			if(isLocalAbsolutePath(path)) return path;
			if(lcwd.endsWith(file_separator)) return lcwd+path;
			return lcwd+file_separator+path;
		}

		public class LsEntry 
		{
			private  String filename;
			private  String longname;
			private  SftpATTRS attrs;
			internal LsEntry(String filename, String longname, SftpATTRS attrs)
			{
				setFilename(filename);
				setLongname(longname);
				setAttrs(attrs);
			}
			public String getFilename(){return filename;}
			void setFilename(String filename){this.filename = filename;}
			public String getLongname(){return longname;}
			void setLongname(String longname){this.longname = longname;}
			public SftpATTRS getAttrs(){return attrs;}
			void setAttrs(SftpATTRS attrs) {this.attrs = attrs;}
			public override string ToString(){return toString();}
			public String toString(){ return longname; }
		}

		public class InputStreamGet : InputStream
		{
			ChannelSftp sftp;
			SftpProgressMonitor monitor;
			long offset=0;
			bool closed=false;
			int rest_length=0;
			byte[] _data=new byte[1];
			byte[] rest_byte=new byte[1024];
			byte[] handle;
			Header header=new Header();
	
			public InputStreamGet( 
				ChannelSftp sftp,
				byte[] handle,
				SftpProgressMonitor monitor)
			{
				this.sftp=sftp;
				this.handle=handle;
				this.monitor=monitor;
			}

			public override int ReadByte() 
			{
				if(closed)return -1;
				int i=read(_data, 0, 1);
				if (i==-1) { return -1; }
				else 
				{
					return _data[0]&0xff;
				}
			}
			public int Read(byte[] d) 
			{
				if(closed)return -1;
				return Read(d, 0, d.Length);
			}
			public override int Read(byte[] d, int s, int len) 
			{
				if(closed)return -1;
				int i;
				int foo;
				if(d==null){throw new System.NullReferenceException();}
				if(s<0 || len <0 || s+len>d.Length)
				{
					throw new System.IndexOutOfRangeException();
				} 
				if(len==0){ return 0; }

				if(rest_length>0)
				{
					foo=rest_length;
					if(foo>len) foo=len;
					java.System.arraycopy(rest_byte, 0, d, s, foo);
					if(foo!=rest_length)
					{
						java.System.arraycopy(rest_byte, foo, 
							rest_byte, 0, rest_length-foo);
					}
					if(monitor!=null)
					{
						if(!monitor.count(foo))
						{
							close();
							return -1;
						}
					}

					rest_length-=foo;
					return foo;
				}

				if(sftp.buf.buffer.Length-13<len)
				{
					len=sftp.buf.buffer.Length-13;
				}
				if(sftp.server_version==0 && len>1024)
				{
					len=1024; 
				}

				try{sftp.sendREAD(handle, offset, len);}
				catch(Exception e){ throw new System.IO.IOException("error"); }

				header= sftp.header(sftp.buf, header);
				rest_length=header.length;
				int type=header.type;
				int id=header.rid;
				
				if(type!=SSH_FXP_STATUS && type!=SSH_FXP_DATA)
				{ 
					throw new System.IO.IOException("error");
				}
				if(type==SSH_FXP_STATUS)
				{
					sftp.buf.rewind();
					sftp.fill(sftp.buf.buffer, 0, rest_length);
					i=sftp.buf.getInt();    
					rest_length=0;
					if(i==SSH_FX_EOF)
					{
						close();
						return -1;
					}
					//throwStatusError(buf, i);
					throw new System.IO.IOException("error");
				}
				sftp.buf.rewind();
				sftp.fill(sftp.buf.buffer, 0, 4);
				i=sftp.buf.getInt(); rest_length-=4;
				
				offset+=rest_length;
				foo=i;
				if(foo>0)
				{
					int bar=rest_length;
					if(bar>len)
					{
						bar=len;
					}
					i=sftp.io.ins.read(d, s, bar);
					if(i<0)
					{
						return -1;
					}
					rest_length-=i;
  
					if(rest_length>0)
					{
						if(rest_byte.Length<rest_length)
						{
							rest_byte=new byte[rest_length];
						}
						int _s=0;
						int _len=rest_length;
						int j;
						while(_len>0)
						{
							j=sftp.io.ins.read(rest_byte, _s, _len);
							if(j<=0)break;
							_s+=j;
							_len-=j;
						}
					}

					if(monitor!=null)
					{
						if(!monitor.count(i))
						{
							close();
							return -1;
						}
					}
					return i;
				}
				return 0; // ??
			}
			public override void Close()
			{
				if(closed)return;
				closed=true;
				/*
						while(rest_length>0){
						  int foo=rest_length;
						  if(foo>buf.buffer.length){
							foo=buf.buffer.length;
						  }
						  io.in.read(buf.buffer, 0, foo);
						  rest_length-=foo;
						}
						*/
				if(monitor!=null)monitor.end();
				try{sftp._sendCLOSE(handle, header);}
				catch(Exception e){throw new System.IO.IOException("error");}
			}
		}
	}
}
