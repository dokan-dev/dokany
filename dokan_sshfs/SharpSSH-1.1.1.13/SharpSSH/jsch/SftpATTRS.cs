using System;
using System.Text;

namespace Tamir.SharpSsh.jsch
{
	/* -*-mode:java; c-basic-offset:2; -*- */
	/*
	Copyright (c) 2002,2003,2004 ymnk, JCraft,Inc. All rights reserved.

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

	/*
	  uint32   flags
	  uint64   size           present only if flag SSH_FILEXFER_ATTR_SIZE
	  uint32   uid            present only if flag SSH_FILEXFER_ATTR_UIDGID
	  uint32   gid            present only if flag SSH_FILEXFER_ATTR_UIDGID
	  uint32   permissions    present only if flag SSH_FILEXFER_ATTR_PERMISSIONS
	  uint32   atime          present only if flag SSH_FILEXFER_ACMODTIME
	  uint32   mtime          present only if flag SSH_FILEXFER_ACMODTIME
	  uint32   extended_count present only if flag SSH_FILEXFER_ATTR_EXTENDED
	  string   extended_type
	  string   extended_data
		...      more extended data (extended_type - extended_data pairs),
				 so that number of pairs equals extended_count
	*/
	public class SftpATTRS 
	{

		static  int S_ISUID = 04000; // set user ID on execution
		static  int S_ISGID = 02000; // set group ID on execution
		static  int S_ISVTX = 01000; // sticky bit   ****** NOT DOCUMENTED *****

		static  int S_IRUSR = 00400; // read by owner
		static  int S_IWUSR = 00200; // write by owner
		static  int S_IXUSR = 00100; // execute/search by owner
		static  int S_IREAD = 00400; // read by owner
		static  int S_IWRITE= 00200; // write by owner
		static  int S_IEXEC = 00100; // execute/search by owner

		static  int S_IRGRP = 00040; // read by group
		static  int S_IWGRP = 00020; // write by group
		static  int S_IXGRP = 00010; // execute/search by group

		static  int S_IROTH = 00004; // read by others
		static  int S_IWOTH = 00002; // write by others
		static  int S_IXOTH = 00001; // execute/search by others

		private static int pmask = 0xFFF;

		public String getPermissionsString() 
		{
			StringBuilder buf = new StringBuilder(10);

			if(isDir()) buf.Append('d');
			else if(isLink()) buf.Append('l');
			else buf.Append('-');

			if((permissions & S_IRUSR)!=0) buf.Append('r');
			else buf.Append('-');

			if((permissions & S_IWUSR)!=0) buf.Append('w');
			else buf.Append('-');

			if((permissions & S_ISUID)!=0) buf.Append('s');
			else if ((permissions & S_IXUSR)!=0) buf.Append('x');
			else buf.Append('-');

			if((permissions & S_IRGRP)!=0) buf.Append('r');
			else buf.Append('-');

			if((permissions & S_IWGRP)!=0) buf.Append('w');
			else buf.Append('-');

			if((permissions & S_ISGID)!=0) buf.Append('s');
			else if((permissions & S_IXGRP)!=0) buf.Append('x');
			else buf.Append('-');

			if((permissions & S_IROTH) != 0) buf.Append('r');
			else buf.Append('-');

			if((permissions & S_IWOTH) != 0) buf.Append('w');
			else buf.Append('-');

			if((permissions & S_IXOTH) != 0) buf.Append('x');
			else buf.Append('-');
			return (buf.ToString());
		}

		public String  getAtimeString()
		{
			//SimpleDateFormat locale=new SimpleDateFormat();
			//return (locale.format(new Date(atime)));
			//[tamir] use Time_T2DateTime to convert t_time to DateTime
			DateTime d = Util.Time_T2DateTime((uint)atime);
			return d.ToShortDateString();
			
		}

		public String  getMtimeString()
		{
			//[tamir] use Time_T2DateTime to convert t_time to DateTime
			DateTime date= Util.Time_T2DateTime((uint)mtime);
			return (date.ToString());
		}

		public static  int SSH_FILEXFER_ATTR_SIZE=         0x00000001;
		public static  int SSH_FILEXFER_ATTR_UIDGID=       0x00000002;
		public static  int SSH_FILEXFER_ATTR_PERMISSIONS=  0x00000004;
		public static  int SSH_FILEXFER_ATTR_ACMODTIME=    0x00000008;
		public static  uint SSH_FILEXFER_ATTR_EXTENDED=     0x80000000;

		static  int S_IFDIR=0x4000;
		static  int S_IFLNK=0xa000;

		int flags=0;
		long size;
		internal int uid;
		internal int gid;
		int permissions;
		int atime;
		int mtime;
		String[] extended=null;

		private SftpATTRS()
		{
		}

		internal static SftpATTRS getATTR(Buffer buf)
		{
			SftpATTRS attr=new SftpATTRS();	
			attr.flags=buf.getInt();
			if((attr.flags&SSH_FILEXFER_ATTR_SIZE)!=0){ attr.size=buf.getLong(); }
			if((attr.flags&SSH_FILEXFER_ATTR_UIDGID)!=0)
			{
				attr.uid=buf.getInt(); attr.gid=buf.getInt();
			}
			if((attr.flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0)
			{ 
				attr.permissions=buf.getInt();
			}
			if((attr.flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0)
			{ 
				attr.atime=buf.getInt();
			}
			if((attr.flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0)
			{ 
				attr.mtime=buf.getInt(); 
			}
			if((attr.flags&SSH_FILEXFER_ATTR_EXTENDED)!=0)
			{
				int count=buf.getInt();
				if(count>0)
				{
					attr.extended=new String[count*2];
					for(int i=0; i<count; i++)
					{
						attr.extended[i*2]=Util.getString(buf.getString());
						attr.extended[i*2+1]=Util.getString(buf.getString());
					}
				}
			}
			return attr;
		} 

		internal int Length()
		{
			return length();
		}

		internal int length()
		{
			int len=4;

			if((flags&SSH_FILEXFER_ATTR_SIZE)!=0){ len+=8; }
			if((flags&SSH_FILEXFER_ATTR_UIDGID)!=0){ len+=8; }
			if((flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0){ len+=4; }
			if((flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0){ len+=8; }
			if((flags&SSH_FILEXFER_ATTR_EXTENDED)!=0)
			{
				len+=4;
				int count=extended.Length/2;
				if(count>0)
				{
					for(int i=0; i<count; i++)
					{
						len+=4; len+=extended[i*2].Length;
						len+=4; len+=extended[i*2+1].Length;
					}
				}
			}
			return len;
		}

		internal void dump(Buffer buf)
		{
			buf.putInt(flags);
			if((flags&SSH_FILEXFER_ATTR_SIZE)!=0){ buf.putLong(size); }
			if((flags&SSH_FILEXFER_ATTR_UIDGID)!=0)
			{
				buf.putInt(uid); buf.putInt(gid);
			}
			if((flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0)
			{ 
				buf.putInt(permissions);
			}
			if((flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0){ buf.putInt(atime); }
			if((flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0){ buf.putInt(mtime); }
			if((flags&SSH_FILEXFER_ATTR_EXTENDED)!=0)
			{
				int count=extended.Length/2;
				if(count>0)
				{
					for(int i=0; i<count; i++)
					{
						buf.putString(Util.getBytes(extended[i*2]));
						buf.putString(Util.getBytes(extended[i*2+1]));
					}
				}
			}
		}
		internal void setFLAGS(int flags)
		{
			this.flags=flags;
		}
		public void setSIZE(long size)
		{
			flags|=SSH_FILEXFER_ATTR_SIZE;
			this.size=size;
		}
		public void setUIDGID(int uid, int gid)
		{
			flags|=SSH_FILEXFER_ATTR_UIDGID;
			this.uid=uid;
			this.gid=gid;
		}
		public void setACMODTIME(int atime, int mtime)
		{
			flags|=SSH_FILEXFER_ATTR_ACMODTIME;
			this.atime=atime;
			this.mtime=mtime;
		}
		public void setPERMISSIONS(int permissions)
		{
			flags|=SSH_FILEXFER_ATTR_PERMISSIONS;
			permissions=(this.permissions&~pmask)|(permissions&pmask);
			this.permissions=permissions;
		}

		public bool isDir()
		{
			return ((flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0 && 
				((permissions&S_IFDIR)==S_IFDIR));
		}      
		public bool isLink()
		{
			return ((flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0 && 
				((permissions&S_IFLNK)==S_IFLNK));
		}
		public int getFlags() { return flags; }
		public long getSize() { return size; }
		public int getUId() { return uid; }
		public int getGId() { return gid; }
		public int getPermissions() { return permissions; }
		public int getATime() { return atime; }
		public int getMTime() { return mtime; }
		public String[] getExtended() { return extended; }

		public String toString() 
		{
			return (getPermissionsString()+" "+getUId()+" "+getGId()+" "+getSize()+" "+getMtimeString());
		}
		
		public override string ToString()
		{
			return toString();
		}
		/*
		public String toString(){
		  return (((flags&SSH_FILEXFER_ATTR_SIZE)!=0) ? ("size:"+size+" ") : "")+
				 (((flags&SSH_FILEXFER_ATTR_UIDGID)!=0) ? ("uid:"+uid+",gid:"+gid+" ") : "")+
				 (((flags&SSH_FILEXFER_ATTR_PERMISSIONS)!=0) ? ("permissions:0x"+Integer.toHexString(permissions)+" ") : "")+
				 (((flags&SSH_FILEXFER_ATTR_ACMODTIME)!=0) ? ("atime:"+atime+",mtime:"+mtime+" ") : "")+
				 (((flags&SSH_FILEXFER_ATTR_EXTENDED)!=0) ? ("extended:?"+" ") : "");
		}
		*/
	}

}
