using System;
using System.IO;
using Tamir.Streams;
using Tamir.SharpSsh.jsch;
using Tamir.SharpSsh.jsch.examples;

namespace Tamir
{
	/// <summary>
	/// Summary description for Main.
	/// </summary>
	public class MainClass
	{
		public static void Main()
		{
			Console.WriteLine("sharpSsh 1.0");

			//testSsh();
			//test();
			//testSig();
			//testSig2();
			//testSigFromJava();
			//testDump();
			//testDumpBase64();
			//testHMacMD5();
			testExamples();
			//jarAndScp();


		}
		public static void test()
		{
			JSch jsch = new JSch();
			DH dh1 = null;
			DH dh2 = null;
			try
			{
				Type t=Type.GetType(jsch.getConfig("dh"));
				dh1=(DH)(Activator.CreateInstance(t));
				dh1.init();
				dh2=(DH)(Activator.CreateInstance(t));
				dh2.init();
			}
			catch(Exception ee)
			{
				Console.WriteLine(ee);
			}

			dh1.setP(DHG1.p);
			dh1.setG(DHG1.g);
			dh2.setP(DHG1.p);
			dh2.setG(DHG1.g);

			// The client responds with:
			// byte  SSH_MSG_KEXDH_INIT(30)
			// mpint e <- g^x mod p
			//         x is a random number (1 < x < (p-1)/2)

			byte[] e=dh1.getE();
			byte[] f=dh2.getE();
			Console.WriteLine("Private1 = {0}", hex(e));
			Console.WriteLine();
			Console.WriteLine("Private2 = {0}", hex(f));
			Console.WriteLine();
			dh1.setF(f);
			dh2.setF(e);
			byte[] k1 = dh1.getK();
			byte[] k2 = dh2.getK();
			Console.WriteLine("Public1 = {0}", hex(k1));
			Console.WriteLine();
			Console.WriteLine("Public2 = {0}", hex(k2));
			Console.WriteLine();
		}

		public static void testSig()
		{
			byte[] hash = Util.getBytes( "Tamir" );
			Tamir.SharpSsh.jsch.jce.SignatureRSA enc_rsa = new Tamir.SharpSsh.jsch.jce.SignatureRSA();
			Tamir.SharpSsh.jsch.jce.SignatureRSA dec_rsa = new Tamir.SharpSsh.jsch.jce.SignatureRSA();
			Tamir.SharpSsh.jsch.jce.KeyPairGenRSA gen = new Tamir.SharpSsh.jsch.jce.KeyPairGenRSA();
			gen.init(512);
			
			enc_rsa.init();
			enc_rsa.setPrvKey(gen.KeyInfo);
			enc_rsa.update(hash);
			byte[] sig = enc_rsa.sign();

			dump(gen.getE(), gen.getN(), sig, hash);

			dec_rsa.init();
			dec_rsa.setPubKey(gen.getE(), gen.getN());
			dec_rsa.update(hash);
			
			

			Console.WriteLine( dec_rsa.verify(sig) );
		}

		public static void testSigFromJava()
		{
			FileStream fs = File.OpenRead("e.bin");
			byte[] e = new byte[fs.Length];
			fs.Read(e, 0, e.Length);
			fs.Close();
			
			fs = File.OpenRead("n.bin");
			byte[] n = new byte[fs.Length];
			fs.Read(n, 0, n.Length);
			fs.Close();

			fs = File.OpenRead("sig.bin");
			byte[] sig = new byte[fs.Length];
			fs.Read(sig, 0, sig.Length);
			fs.Close();

			Console.Write("E: ");
			Console.WriteLine( hex(e) );
			Console.Write("N: ");
			Console.WriteLine( hex(n) );
			Console.Write("SIG: ");
			Console.WriteLine( hex(sig) );

			byte[] hash = Util.getBytes( "Tamir" );
			Tamir.SharpSsh.jsch.jce.SignatureRSA dec_rsa = new Tamir.SharpSsh.jsch.jce.SignatureRSA();
			dec_rsa.init();
			dec_rsa.setPubKey(e, n);
			dec_rsa.update(hash);
			Console.WriteLine( dec_rsa.verify(sig) );


		}

		public static void dump(byte[] e, byte[] n, byte[] sig, byte[] hash)
		{
			String fname = "e.bin";
			if(File.Exists(fname)) File.Delete(fname);
			FileStream fs = File.OpenWrite(fname);
			fs.Write(e, 0, e.Length);
			fs.Close();


			fname = "n.bin";
			if(File.Exists(fname)) File.Delete(fname);
			fs = File.OpenWrite(fname);
			fs.Write(n, 0, n.Length);
			fs.Close();


			fname = "sig.bin";
			if(File.Exists(fname)) File.Delete(fname);
			fs = File.OpenWrite(fname);
			fs.Write(sig, 0, sig.Length);
			fs.Close();

			fname = "hash.bin";
			if(File.Exists(fname)) File.Delete(fname);
			fs = File.OpenWrite(fname);
			fs.Write(hash, 0, hash.Length);
			fs.Close();

			
			Console.Write("E: ");
			Console.WriteLine( hex(e) );
			Console.WriteLine();
			Console.Write("N: ");
			Console.WriteLine( hex(n) );
			Console.WriteLine();
			Console.Write("SIG: ");
			Console.WriteLine( hex(sig) );
			Console.WriteLine();
			Console.Write("HASH: ");
			Console.WriteLine( hex(hash) );
			Console.WriteLine();
		}

		public static void testDump()
		{
			String fname = "e.bin";
			FileStream fs = File.OpenRead(fname);
			byte[] e = new byte[fs.Length];
			fs.Read(e, 0, e.Length);
			fs.Close();
			
			fname = "n.bin";
			fs = File.OpenRead(fname);
			byte[] n = new byte[fs.Length];
			fs.Read(n, 0, n.Length);
			fs.Close();

			fname = "sig.bin";
			fs = File.OpenRead(fname);
			byte[] sig = new byte[fs.Length];
			fs.Read(sig, 0, sig.Length);
			fs.Close();

			fname = "hash.bin";
			fs = File.OpenRead(fname);
			byte[] hash = new byte[fs.Length];
			fs.Read(hash, 0, hash.Length);
			fs.Close();

			print("E", e);
			print("N", n);
			print("SIG", sig);
			print("HASH", hash);

			Tamir.SharpSsh.jsch.jce.SignatureRSA dec_rsa = new Tamir.SharpSsh.jsch.jce.SignatureRSA();
			dec_rsa.init();
			dec_rsa.setPubKey(e, n);
			dec_rsa.update(hash);
			
			Console.WriteLine( dec_rsa.verify(sig) );
			Console.WriteLine();
		}

		public static void testDumpBase64()
		{
			String fname = "e.bin";
			StreamReader fs = File.OpenText(fname);
			string base64 = fs.ReadToEnd();
			byte[] e = Convert.FromBase64String( base64 );
			fs.Close();
			
			fname = "n.bin";
			fs = File.OpenText(fname);
			base64 = fs.ReadToEnd();
			byte[] n = Convert.FromBase64String( base64 );
			fs.Close();

			fname = "sig.bin";
			fs = File.OpenText(fname);
			base64 = fs.ReadToEnd();
			byte[] sig = Convert.FromBase64String( base64 );
			fs.Close();

			fname = "hash.bin";
			fs = File.OpenText(fname);
			base64 = fs.ReadToEnd();
			byte[] hash = Convert.FromBase64String( base64 );
			fs.Close();

			print("E", e);
			print("N", n);
			print("SIG", sig);
			print("HASH", hash);

			Tamir.SharpSsh.jsch.jce.SignatureRSA dec_rsa = new Tamir.SharpSsh.jsch.jce.SignatureRSA();
			dec_rsa.init();
			dec_rsa.setPubKey(e, n);
			dec_rsa.update(hash);
			
			Console.WriteLine( dec_rsa.verify(sig) );
			Console.WriteLine();
		}

		public static void testHMacMD5()
		{
			byte[] msg = Util.getBytes("Tamir");
			byte[] key = new byte[]{1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6};
			Tamir.SharpSsh.jsch.jce.HMACMD5 md5 = new Tamir.SharpSsh.jsch.jce.HMACMD5();
			md5.init( key );
			md5.update(msg, 0, msg.Length);
			byte[] hash = md5.doFinal();
			Console.WriteLine(hex(hash));
		}

		public static void testExamples()
		{
			//String[] arg = new string[]{"rsa", "key", "sharpSSH"};
			//KeyGen.RunExample(arg);

//			String[] arg = new string[]{"key"};
//			ChangePassphrase.RunExample(arg);

			//UserAuthPubKey.RunExample(null);

			Tamir.SharpSsh.jsch.examples.KnownHosts.RunExample(null);
			
//			String[] arg = new string[]{"C:\\untitled.db", "root@rhclient8:/root/mng/tamir/file"};
//			Tamir.SharpSsh.jsch.examples.ScpTo.RunExample(arg);

//			String[] arg = new string[]{"root@rhclient8:/root/mng/tamir/file", "file.txt"};
//			Tamir.SharpSsh.jsch.examples.ScpFrom.RunExample(arg);

//			Tamir.SharpSsh.jsch.examples.Sftp.RunExample(null);
		}

		public static void jarAndScp()
		{
			TextReader r = File.OpenText( "jarAndScp.txt" );
			string dir = r.ReadLine();
			string host = r.ReadLine();
			string path = r.ReadLine();
			string user = r.ReadLine();
			string pass = r.ReadLine();
			r.Close();
			string file = dir+".jar";
			string jarFile = "\""+dir+".jar\"";
			//dir = "\""+dir+"\"";
			System.Diagnostics.ProcessStartInfo p = new System.Diagnostics.ProcessStartInfo(@"D:\Program Files\Java\jdk1.5.0_03\bin\jar.exe");
			p.WorkingDirectory = Directory.GetParent(dir).FullName;
			p.Arguments = "-cf "+jarFile+" "+ Path.GetFileName(dir);
			p.UseShellExecute = false;
//			p.RedirectStandardOutput = true;
//			p.RedirectStandardError = true;
			System.Diagnostics.Process.Start(p);
			System.Diagnostics.Process pr = new System.Diagnostics.Process();
			pr.StartInfo = p;
			pr.Start();
			pr.WaitForExit();

			String[] arg = new string[]{file, user+"@"+host+":"+path+Path.GetFileName(file)};
			//Tamir.SharpSsh.Scp.To(file, host, path+Path.GetFileName(file), user, pass);
		}

		public static void print(string name, byte[] data)
		{
			Console.WriteLine();
			Console.Write(name+": ");
			Console.WriteLine( hex(data) );
			Console.WriteLine();
		}

		public static string hex(byte[] arr)
		{
			string hex = "0x";
			for(int i=0;i<arr.Length; i++)
			{
				string mbyte = arr[i].ToString("X");
				if (mbyte.Length == 1)
					mbyte = "0"+mbyte;
				hex += mbyte;
			}
			return hex;
		}

		public static byte[] reverse(byte[] arr)
		{
			byte[] tmp = new byte[arr.Length];
			for(int i=0; i<arr.Length; i++)
			{
				tmp[i] = arr[ arr.Length-1-i];
			}
			return tmp;
		}

  /**
   * Utility method to delete the leading zeros from the modulus.
   * @param a modulus
   * @return modulus
   */
		public static byte[] stripLeadingZeros(byte[] a) 
		{
			int lastZero = -1;
			for (int i = 0; i < a.Length; i++) 
			{
				if (a[i] == 0) 
				{
					lastZero = i;
				}
				else 
				{
					break;
				}
			}
			lastZero++;
			byte[] result = new byte[a.Length - lastZero];
			Array.Copy(a, lastZero, result, 0, result.Length);
			return result;
		}
	}
}
