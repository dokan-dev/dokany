using System;
using System.Drawing;
using System.Collections;
using System.ComponentModel;
using System.Windows.Forms;
using System.Data;

namespace sharpSshTest.jsch_samples
{
	/// <summary>
	/// Summary description for Form1.
	/// </summary>
	public class InputForm : System.Windows.Forms.Form
	{
		private static InputForm inForm;
		public System.Windows.Forms.TextBox textBox1;
		private System.Windows.Forms.Button btnOK;
		private System.Windows.Forms.Button btnCancel;
		/// <summary>
		/// Required designer variable.
		/// </summary>
		private System.ComponentModel.Container components = null;

		public InputForm()
		{
			InitializeComponent();
		}

		private static InputForm Instance
		{
			get{
				if(inForm==null) inForm = new InputForm();
				return inForm;
			}
		}

		public static string GetUserInput(string title, string devaultValue)
		{
			return GetUserInput(title, devaultValue, false);
		}

		public static string GetUserInput(string title, bool password)
		{
			return GetUserInput(title, "", password);
		}


		public static string GetUserInput(string title, string devaultValue, bool password)
		{
			Instance.Text = title;
			Instance.textBox1.Text = devaultValue;
			Instance.PasswordField = password;
			if(Instance.PromptForInput())
			{
				return Instance.textBox1.Text;
			}
			else
			{
				throw new Exception("Canceled by user");
			}
		}

		public static string GetFileFromUser(string msg)
		{
			OpenFileDialog chooser = new OpenFileDialog();
			chooser.Title = msg;
			DialogResult returnVal = chooser.ShowDialog();
			if(returnVal == DialogResult.OK) 
			{
				return chooser.FileName;
			}
			else
			{
				throw new Exception("Canceled by user");
			}
		}

		public static bool PromptYesNo(String str)
		{
			DialogResult returnVal = MessageBox.Show(
				str,
				"SharpSSH",
				MessageBoxButtons.YesNo,
				MessageBoxIcon.Warning);
			return (returnVal==DialogResult.Yes);
		}

		public static void ShowMessage(String message)
		{
			MessageBox.Show(
				message,
				"SharpSSH",
				MessageBoxButtons.OK,
				MessageBoxIcon.Asterisk);
		}

		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		protected override void Dispose( bool disposing )
		{
			if( disposing )
			{
				if (components != null) 
				{
					components.Dispose();
				}
			}
			base.Dispose( disposing );
		}

		#region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		private void InitializeComponent()
		{
			this.textBox1 = new System.Windows.Forms.TextBox();
			this.btnOK = new System.Windows.Forms.Button();
			this.btnCancel = new System.Windows.Forms.Button();
			this.SuspendLayout();
			// 
			// textBox1
			// 
			this.textBox1.Location = new System.Drawing.Point(56, 24);
			this.textBox1.Name = "textBox1";
			this.textBox1.Size = new System.Drawing.Size(160, 20);
			this.textBox1.TabIndex = 0;
			this.textBox1.Text = "";
			// 
			// btnOK
			// 
			this.btnOK.Location = new System.Drawing.Point(56, 64);
			this.btnOK.Name = "btnOK";
			this.btnOK.TabIndex = 1;
			this.btnOK.Text = "OK";
			this.btnOK.Click += new System.EventHandler(this.btnOK_Click);
			// 
			// btnCancel
			// 
			this.btnCancel.DialogResult = System.Windows.Forms.DialogResult.Cancel;
			this.btnCancel.Location = new System.Drawing.Point(144, 64);
			this.btnCancel.Name = "btnCancel";
			this.btnCancel.TabIndex = 2;
			this.btnCancel.Text = "Cancel";
			this.btnCancel.Click += new System.EventHandler(this.btnCancel_Click);
			// 
			// InputForm
			// 
			this.AcceptButton = this.btnOK;
			this.AutoScaleBaseSize = new System.Drawing.Size(5, 13);
			this.CancelButton = this.btnCancel;
			this.ClientSize = new System.Drawing.Size(264, 110);
			this.Controls.Add(this.btnCancel);
			this.Controls.Add(this.btnOK);
			this.Controls.Add(this.textBox1);
			this.Name = "InputForm";
			this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
			this.Text = "Form1";
			this.ResumeLayout(false);

		}
		#endregion

		bool okBtnClicked = false;

		private void btnOK_Click(object sender, System.EventArgs e)
		{
			okBtnClicked = true;
			this.Hide();
		}

		private void btnCancel_Click(object sender, System.EventArgs e)
		{
			okBtnClicked = false;
			this.textBox1.Text = "";
			this.Hide();			
		}

		public bool PromptForInput()
		{
			this.ShowDialog();
			return okBtnClicked;
		}

		public string getText()
		{
			return textBox1.Text;
		}

		public bool PasswordField
		{
			get
			{
				return (textBox1.PasswordChar.Equals(0));
			}
			set
			{
				if (value)
					textBox1.PasswordChar='*';
				else
					textBox1.PasswordChar=(char)0;
			}
		}
	}
}
