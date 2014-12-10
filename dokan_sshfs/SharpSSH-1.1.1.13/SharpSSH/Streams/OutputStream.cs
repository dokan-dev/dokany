//using System;
//using System.IO;
//
//namespace Tamir.Streams
//{
//	/// <summary>
//	/// Summary description for OutputStream.
//	/// </summary>
//	public abstract class OutputStream : Stream
//	{
//		public override int Read(byte[] buffer, int offset, int count)
//		{
//			return 0;
//		}
//
//		public override int ReadByte()
//		{
//			return 0;
//		}
//
//		public override bool CanRead
//		{
//			get
//			{
//				return false;
//			}
//		}
//		public override bool CanWrite
//		{
//			get
//			{
//				return true;
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
