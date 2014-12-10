using System;
using System.IO;

namespace Tamir.Streams
{
	/// <summary>
	/// Summary description for PipedStream.
	/// </summary>
	public class PipedStream : Stream
	{
		PipedInputStream pins;
		PipedOutputStream pouts;

		public PipedStream(PipedInputStream pins, PipedOutputStream pouts)
		{
			this.pins = pins;
			this.pouts = pouts;
		}

		public override int Read(byte[] buffer, int offset, int count)
		{
			return pins.read(buffer, offset, count);
		}

		public override int ReadByte()
		{
			return pins.read();
		}
		
		public override void WriteByte(byte value)
		{
			pouts.write(value);
		}


		public override void Write(byte[] buffer, int offset, int count)
		{
			pouts.write(buffer, offset, count);
		}
		public override void Close()
		{
			base.Close ();
			pins.close();
			pouts.close();
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
				return true;
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
		public override long Seek(long offset, SeekOrigin origin)
		{
			return 0;
		}
	}
}
