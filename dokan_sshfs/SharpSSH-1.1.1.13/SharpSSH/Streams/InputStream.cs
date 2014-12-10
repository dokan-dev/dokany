//using System;
//using System.IO;
//
//namespace Tamir.Streams
//{
//	/// <summary>
//	/// Summary description for InputStream.
//	/// </summary>
//	public abstract class InputStream : Stream
//	{
//		public override void WriteByte(byte value)
//		{
//		}
//
//		public override void Write(byte[] buffer, int offset, int count)
//		{
//		}
//
//		public override bool CanRead
//		{
//			get
//			{
//				return true;
//			}
//		}
//		public override bool CanWrite
//		{
//			get
//			{
//				return false;
//			}
//		}
//		public override bool CanSeek
//		{
//			get
//			{
//				return false;
//			}
//		}
//		public override void Flush()
//		{
//			
//		}
//		public override long Length
//		{
//			get
//			{
//				return 0;
//			}
//		}
//		public override long Position
//		{
//			get
//			{
//				return 0;
//			}
//			set
//			{
//			}
//		}
//		public override void SetLength(long value)
//		{			
//		}
//		public override long Seek(long offset, SeekOrigin origin)
//		{
//			return 0;
//		}
//	}
//}
