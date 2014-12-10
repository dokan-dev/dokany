//using System;
//using System.IO;
//
//namespace Tamir.SharpSsh.jsch
//{
//	public class InputStreamGet : java.io.InputStream
//	{
//		bool closed=false;
//		int rest_length=0;
//		byte[] _data=new byte[1];
//		ChannelSftp sftp;
//		byte[] handle;
//		long[] _offset;
//		int[]_server_version;
//		SftpProgressMonitor monitor;
//	
//		public InputStreamGet( 
//			ChannelSftp sftp,
//			byte[] handle,
//			long[] _offset,
//			int[] _server_version,
//			SftpProgressMonitor monitor)
//		{
//			this.sftp=sftp;
//			this.handle=handle;
//			this._offset=_offset;
//			this._server_version=_server_version;
//			this.monitor=monitor;
//		}
//
//		public override int ReadByte()
//		{
//			int i=read(_data, 0, 1);
//			if (i==-1) { return -1; }
//			else 
//			{
//				return _data[0]&0xff;
//			}
//		}
//		public override int read(byte[] d)
//		{
//			return Read(d, 0, d.Length);
//		}
//		public override int Read(byte[] d, int s, int len)
//		{
//			if(d==null){throw new NullReferenceException();}
//			if(s<0 || len <0 || s+len>d.Length)
//			{
//				throw new ArgumentOutOfRangeException();
//			} 
//			if(len==0){ return 0; }
//
//			if(rest_length>0)
//			{
//				int foo=rest_length;
//				if(foo>len) foo=len;
//				int i=sftp.io.ins.read(d, s, foo);
//				if(i<0)
//				{
//					throw new IOException("error");
//				}
//				rest_length-=i;
//				return i;
//			}
//
//			if(sftp.buf.buffer.length-13<len)
//			{
//				len=sftp.buf.buffer.length-13;
//			}
//			if(sftp.server_version==0 && len>1024)
//			{
//				len=1024; 
//			}
//
//			try{sftp.sendREAD(handle, offset, len);}
//			catch(Exception e){ throw new IOException("error"); }
//
//			sftp.buf.rewind();
//			int i=io.ins.read(buf.buffer, 0, 13);  // 4 + 1 + 4 + 4
//			if(i!=13)
//			{ 
//				throw new IOException("error");
//			}
//
//			rest_length=sftp.buf.getInt();
//			int type=sftp.buf.getByte();  
//			rest_length--;
//			sftp.buf.getInt();        
//			rest_length-=4;
//			if(type!=sftp.SSH_FXP_STATUS && type!=SSH_FXP_DATA)
//			{ 
//				throw new IOException("error");
//			}
//			if(type==sftp.SSH_FXP_STATUS)
//			{
//				i=buf.getInt();    
//				rest_length-=4;
//				sftp.io.ins.read(sftp.buf.buffer, 13, rest_length);
//				rest_length=0;
//				if(i==SSH_FX_EOF)
//				{
//					close();
//					return -1;
//				}
//				//throwStatusError(buf, i);
//				throw new IOException("error");
//			}
//
//			i=buf.getInt();    
//			rest_length-=4;
//			offset+=rest_length;
//			int foo=i;
//			if(foo>0)
//			{
//				int bar=rest_length;
//				if(bar>len)
//				{
//					bar=len;
//				}
//				i=io.ins.read(d, s, bar);
//				if(i<0)
//				{
//					return -1;
//				}
//				rest_length-=i;
//  
//				if(monitor!=null)
//				{
//					if(!monitor.count(i))
//					{
//						return -1;
//					}
//				}
//				return i;
//			}
//			return 0; // ??
//		}
//		public  override void Close()
//		{
//			if(closed)return;
//			closed=true;
//			/*
//			while(rest_length>0){
//			  int foo=rest_length;
//			  if(foo>buf.buffer.length){
//				foo=buf.buffer.length;
//			  }
//			  io.in.read(buf.buffer, 0, foo);
//			  rest_length-=foo;
//			}
//			*/
//			if(monitor!=null)monitor.end();
//			try{sftp._sendCLOSE(handle);}
//			catch(Exception e){throw new IOException("error");}
//		}
//	}
//}
