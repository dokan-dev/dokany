using System;
using IO = System.IO;

namespace Tamir.SharpSsh.java.io
{
	/// <summary>
	/// Summary description for InputStream.
	/// </summary>
	public abstract class InputStream : IO.Stream
	{
		public virtual int read(byte[] buffer, int offset, int count)
		{
			return this.Read(buffer, offset, count);
		}

		public virtual int read(byte[] buffer)
		{
			return this.Read(buffer, 0, buffer.Length);
		}
		
		public virtual int read()
		{
			return this.ReadByte();
		}

		public virtual void close()
		{
			Close();
		}

		public override void WriteByte(byte value)
		{
		}

		public override void Write(byte[] buffer, int offset, int count)
		{
		}

		public override bool CanRead
		{
			get
			{
				return true;
			}
		}
		public override bool CanWrite
		{
			get
			{
				return false;
			}
		}
		public override bool CanSeek
		{
			get
			{
				return false;
			}
		}
		public override void Flush()
		{
			
		}
		public override long Length
		{
			get
			{
				return 0;
			}
		}
		public override long Position
		{
			get
			{
				return 0;
			}
			set
			{
			}
		}
		public override void SetLength(long value)
		{			
		}
		public override long Seek(long offset, IO.SeekOrigin origin)
		{
			return 0;
		}

		public long skip(long len)
		{
			//Seek doesn't work
			//return Seek(offset, IO.SeekOrigin.Current);
			int i=0;
			int count = 0;
			byte[] buf = new byte[len];
			while(len>0)
			{
				i=Read(buf, count, (int)len);//tamir: possible lost of pressision
				if(i<=0)
				{
					throw new Exception("inputstream is closed");
					//return (s-foo)==0 ? i : s-foo;
				}
				count+=i;
				len-=i;
			}
			return count;
		}
	}
}
