using System;
using IO = System.IO;

namespace Tamir.SharpSsh.java.io
{
	/// <summary>
	/// Summary description for Stream.
	/// </summary>
	public class JStream : IO.Stream
	{
		internal IO.Stream s;
		public JStream(IO.Stream s)
		{
			this.s = s;
		}

		public override int Read(byte[] buffer, int offset, int count)
		{
			return s.Read(buffer, offset, count);
		}

		public override int ReadByte()
		{
			return s.ReadByte();
		}

		public int read(byte[] buffer, int offset, int count)
		{
			return Read(buffer, offset, count);
		}

		public int read(byte[] buffer)
		{
			return Read(buffer, 0, buffer.Length);
		}
		
		public int read()
		{
			return ReadByte();
		}

		public void close()
		{
			this.Close();
		}

		public override void Close()
		{
			s.Close ();
		}

		public override void WriteByte(byte value)
		{
			s.WriteByte(value);
		}

		public override void Write(byte[] buffer, int offset, int count)
		{
			s.Write(buffer, offset, count);
		}

		public void write(byte[] buffer, int offset, int count)
		{
			Write(buffer, offset, count);
		}
		
		public void write(byte[] buffer)
		{
			Write(buffer, 0, buffer.Length);
		}

		public override bool CanRead
		{
			get {return s.CanRead;}
		}
		public override bool CanWrite
		{
			get
			{
				return s.CanWrite;
			}
		}
		public override bool CanSeek
		{
			get
			{
				return s.CanSeek;
			}
		}
		public override void Flush()
		{
			s.Flush();
		}
		public override long Length
		{
			get
			{
				return s.Length;
			}
		}
		public override long Position
		{
			get
			{
				return s.Position;
			}
			set
			{
				s.Position = value;
			}
		}
		public override void SetLength(long value)
		{	
			s.SetLength(value);
		}
		public override long Seek(long offset, IO.SeekOrigin origin)
		{
			return s.Seek(offset, origin);
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

		public int available()
		{
			if(s is Tamir.Streams.PipedInputStream)
			{
				return ((Tamir.Streams.PipedInputStream)s).available();
			}
			throw new Exception("JStream.available() -- Method not implemented");
		}

		public void flush()
		{
			s.Flush();
		}
	}
}
