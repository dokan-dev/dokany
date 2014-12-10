namespace DokanSSHFS
{
    partial class SettingForm
    {
        /// <summary>
        /// 必要なデザイナ変数です。
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// 使用中のリソースをすべてクリーンアップします。
        /// </summary>
        /// <param name="disposing">マネージ リソースが破棄される場合 true、破棄されない場合は false です。</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows フォーム デザイナで生成されたコード

        /// <summary>
        /// デザイナ サポートに必要なメソッドです。このメソッドの内容を
        /// コード エディタで変更しないでください。
        /// </summary>
        private void InitializeComponent()
        {
            this.components = new System.ComponentModel.Container();
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(SettingForm));
            this.label1 = new System.Windows.Forms.Label();
            this.host = new System.Windows.Forms.TextBox();
            this.label2 = new System.Windows.Forms.Label();
            this.port = new System.Windows.Forms.TextBox();
            this.label3 = new System.Windows.Forms.Label();
            this.user = new System.Windows.Forms.TextBox();
            this.openFileDialog1 = new System.Windows.Forms.OpenFileDialog();
            this.root = new System.Windows.Forms.TextBox();
            this.label4 = new System.Windows.Forms.Label();
            this.connect = new System.Windows.Forms.Button();
            this.cancel = new System.Windows.Forms.Button();
            this.usePassword = new System.Windows.Forms.RadioButton();
            this.password = new System.Windows.Forms.TextBox();
            this.usePrivateKey = new System.Windows.Forms.RadioButton();
            this.privatekey = new System.Windows.Forms.TextBox();
            this.open = new System.Windows.Forms.Button();
            this.notifyIcon1 = new System.Windows.Forms.NotifyIcon(this.components);
            this.notifyMenu = new System.Windows.Forms.ContextMenuStrip(this.components);
            this.exit = new System.Windows.Forms.ToolStripMenuItem();
            this.unmount = new System.Windows.Forms.ToolStripMenuItem();
            this.mount = new System.Windows.Forms.ToolStripMenuItem();
            this.label5 = new System.Windows.Forms.Label();
            this.save = new System.Windows.Forms.Button();
            this.delete = new System.Windows.Forms.Button();
            this.settingNames = new System.Windows.Forms.ComboBox();
            this.passphrase = new System.Windows.Forms.TextBox();
            this.label6 = new System.Windows.Forms.Label();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPage1 = new System.Windows.Forms.TabPage();
            this.drive = new System.Windows.Forms.ComboBox();
            this.tabPage2 = new System.Windows.Forms.TabPage();
            this.disableCache = new System.Windows.Forms.CheckBox();
            this.withoutOfflineAttribute = new System.Windows.Forms.CheckBox();
            this.label7 = new System.Windows.Forms.Label();
            this.notifyMenu.SuspendLayout();
            this.tabControl1.SuspendLayout();
            this.tabPage1.SuspendLayout();
            this.tabPage2.SuspendLayout();
            this.SuspendLayout();
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(3, 11);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(29, 12);
            this.label1.TabIndex = 0;
            this.label1.Text = "Host";
            // 
            // host
            // 
            this.host.Location = new System.Drawing.Point(38, 9);
            this.host.Name = "host";
            this.host.Size = new System.Drawing.Size(278, 19);
            this.host.TabIndex = 4;
            // 
            // label2
            // 
            this.label2.AutoSize = true;
            this.label2.Location = new System.Drawing.Point(218, 47);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(26, 12);
            this.label2.TabIndex = 2;
            this.label2.Text = "Port";
            // 
            // port
            // 
            this.port.Location = new System.Drawing.Point(247, 44);
            this.port.Name = "port";
            this.port.Size = new System.Drawing.Size(68, 19);
            this.port.TabIndex = 6;
            this.port.Text = "22";
            // 
            // label3
            // 
            this.label3.AutoSize = true;
            this.label3.Location = new System.Drawing.Point(3, 46);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(29, 12);
            this.label3.TabIndex = 4;
            this.label3.Text = "User";
            // 
            // user
            // 
            this.user.Location = new System.Drawing.Point(39, 44);
            this.user.Name = "user";
            this.user.Size = new System.Drawing.Size(173, 19);
            this.user.TabIndex = 5;
            // 
            // openFileDialog1
            // 
            this.openFileDialog1.FileName = "openFileDialog1";
            // 
            // root
            // 
            this.root.Location = new System.Drawing.Point(92, 181);
            this.root.Name = "root";
            this.root.Size = new System.Drawing.Size(224, 19);
            this.root.TabIndex = 13;
            this.root.Text = "/";
            // 
            // label4
            // 
            this.label4.AutoSize = true;
            this.label4.Location = new System.Drawing.Point(14, 184);
            this.label4.Name = "label4";
            this.label4.Size = new System.Drawing.Size(66, 12);
            this.label4.TabIndex = 11;
            this.label4.Text = "Server Root";
            // 
            // connect
            // 
            this.connect.Location = new System.Drawing.Point(198, 304);
            this.connect.Name = "connect";
            this.connect.Size = new System.Drawing.Size(75, 23);
            this.connect.TabIndex = 15;
            this.connect.Text = "Connect";
            this.connect.UseVisualStyleBackColor = true;
            this.connect.Click += new System.EventHandler(this.connect_Click);
            // 
            // cancel
            // 
            this.cancel.Location = new System.Drawing.Point(279, 304);
            this.cancel.Name = "cancel";
            this.cancel.Size = new System.Drawing.Size(75, 23);
            this.cancel.TabIndex = 16;
            this.cancel.Text = "Cancel";
            this.cancel.UseVisualStyleBackColor = true;
            this.cancel.Click += new System.EventHandler(this.cancel_Click);
            // 
            // usePassword
            // 
            this.usePassword.AutoSize = true;
            this.usePassword.Checked = true;
            this.usePassword.Location = new System.Drawing.Point(5, 79);
            this.usePassword.Name = "usePassword";
            this.usePassword.Size = new System.Drawing.Size(72, 16);
            this.usePassword.TabIndex = 7;
            this.usePassword.TabStop = true;
            this.usePassword.Text = "Password";
            this.usePassword.UseVisualStyleBackColor = true;
            this.usePassword.CheckedChanged += new System.EventHandler(this.usePassword_CheckedChanged);
            // 
            // password
            // 
            this.password.Location = new System.Drawing.Point(92, 79);
            this.password.Name = "password";
            this.password.Size = new System.Drawing.Size(224, 19);
            this.password.TabIndex = 8;
            this.password.UseSystemPasswordChar = true;
            // 
            // usePrivateKey
            // 
            this.usePrivateKey.AutoSize = true;
            this.usePrivateKey.Location = new System.Drawing.Point(5, 115);
            this.usePrivateKey.Name = "usePrivateKey";
            this.usePrivateKey.Size = new System.Drawing.Size(61, 16);
            this.usePrivateKey.TabIndex = 9;
            this.usePrivateKey.Text = "Identity";
            this.usePrivateKey.UseVisualStyleBackColor = true;
            this.usePrivateKey.CheckedChanged += new System.EventHandler(this.usePrivateKey_CheckedChanged);
            // 
            // privatekey
            // 
            this.privatekey.Enabled = false;
            this.privatekey.Location = new System.Drawing.Point(92, 112);
            this.privatekey.Name = "privatekey";
            this.privatekey.Size = new System.Drawing.Size(172, 19);
            this.privatekey.TabIndex = 10;
            // 
            // open
            // 
            this.open.Enabled = false;
            this.open.Location = new System.Drawing.Point(270, 110);
            this.open.Name = "open";
            this.open.Size = new System.Drawing.Size(46, 23);
            this.open.TabIndex = 11;
            this.open.Text = "Open";
            this.open.UseVisualStyleBackColor = true;
            this.open.Click += new System.EventHandler(this.open_Click);
            // 
            // notifyIcon1
            // 
            this.notifyIcon1.ContextMenuStrip = this.notifyMenu;
            this.notifyIcon1.Icon = ((System.Drawing.Icon)(resources.GetObject("notifyIcon1.Icon")));
            this.notifyIcon1.Text = "SSHFS";
            this.notifyIcon1.Visible = true;
            // 
            // notifyMenu
            // 
            this.notifyMenu.Items.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.exit,
            this.unmount,
            this.mount});
            this.notifyMenu.Name = "Exit";
            this.notifyMenu.ShowImageMargin = false;
            this.notifyMenu.Size = new System.Drawing.Size(91, 70);
            // 
            // exit
            // 
            this.exit.Name = "exit";
            this.exit.Size = new System.Drawing.Size(90, 22);
            this.exit.Text = "Exit";
            this.exit.Click += new System.EventHandler(this.exit_Click);
            // 
            // unmount
            // 
            this.unmount.Name = "unmount";
            this.unmount.Size = new System.Drawing.Size(90, 22);
            this.unmount.Text = "Unmount";
            this.unmount.Visible = false;
            this.unmount.Click += new System.EventHandler(this.unmount_Click);
            // 
            // mount
            // 
            this.mount.Name = "mount";
            this.mount.Size = new System.Drawing.Size(90, 22);
            this.mount.Text = "Mount";
            this.mount.Click += new System.EventHandler(this.mount_Click);
            // 
            // label5
            // 
            this.label5.AutoSize = true;
            this.label5.Location = new System.Drawing.Point(45, 215);
            this.label5.Name = "label5";
            this.label5.Size = new System.Drawing.Size(32, 12);
            this.label5.TabIndex = 16;
            this.label5.Text = "Drive";
            // 
            // save
            // 
            this.save.Location = new System.Drawing.Point(238, 7);
            this.save.Name = "save";
            this.save.Size = new System.Drawing.Size(53, 23);
            this.save.TabIndex = 2;
            this.save.Text = "Save";
            this.save.UseVisualStyleBackColor = true;
            this.save.Click += new System.EventHandler(this.save_Click);
            // 
            // delete
            // 
            this.delete.Location = new System.Drawing.Point(297, 7);
            this.delete.Name = "delete";
            this.delete.Size = new System.Drawing.Size(53, 23);
            this.delete.TabIndex = 3;
            this.delete.Text = "Del";
            this.delete.UseVisualStyleBackColor = true;
            this.delete.Click += new System.EventHandler(this.delete_Click);
            // 
            // settingNames
            // 
            this.settingNames.FormattingEnabled = true;
            this.settingNames.Items.AddRange(new object[] {
            "New Setting"});
            this.settingNames.Location = new System.Drawing.Point(57, 7);
            this.settingNames.Name = "settingNames";
            this.settingNames.Size = new System.Drawing.Size(175, 20);
            this.settingNames.TabIndex = 1;
            this.settingNames.SelectedIndexChanged += new System.EventHandler(this.settingNames_SelectedIndexChanged);
            // 
            // passphrase
            // 
            this.passphrase.Location = new System.Drawing.Point(92, 138);
            this.passphrase.Name = "passphrase";
            this.passphrase.Size = new System.Drawing.Size(223, 19);
            this.passphrase.TabIndex = 12;
            // 
            // label6
            // 
            this.label6.AutoSize = true;
            this.label6.Location = new System.Drawing.Point(21, 141);
            this.label6.Name = "label6";
            this.label6.Size = new System.Drawing.Size(64, 12);
            this.label6.TabIndex = 22;
            this.label6.Text = "Passphrase";
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPage1);
            this.tabControl1.Controls.Add(this.tabPage2);
            this.tabControl1.Location = new System.Drawing.Point(14, 35);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(340, 263);
            this.tabControl1.TabIndex = 23;
            // 
            // tabPage1
            // 
            this.tabPage1.Controls.Add(this.drive);
            this.tabPage1.Controls.Add(this.label1);
            this.tabPage1.Controls.Add(this.host);
            this.tabPage1.Controls.Add(this.label6);
            this.tabPage1.Controls.Add(this.label2);
            this.tabPage1.Controls.Add(this.passphrase);
            this.tabPage1.Controls.Add(this.port);
            this.tabPage1.Controls.Add(this.label3);
            this.tabPage1.Controls.Add(this.open);
            this.tabPage1.Controls.Add(this.user);
            this.tabPage1.Controls.Add(this.label5);
            this.tabPage1.Controls.Add(this.usePassword);
            this.tabPage1.Controls.Add(this.password);
            this.tabPage1.Controls.Add(this.usePrivateKey);
            this.tabPage1.Controls.Add(this.privatekey);
            this.tabPage1.Controls.Add(this.root);
            this.tabPage1.Controls.Add(this.label4);
            this.tabPage1.Location = new System.Drawing.Point(4, 21);
            this.tabPage1.Name = "tabPage1";
            this.tabPage1.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage1.Size = new System.Drawing.Size(332, 238);
            this.tabPage1.TabIndex = 0;
            this.tabPage1.Text = "Site";
            this.tabPage1.UseVisualStyleBackColor = true;
            // 
            // drive
            // 
            this.drive.FormattingEnabled = true;
            this.drive.Items.AddRange(new object[] {
            "A",
            "B",
            "C",
            "D",
            "E",
            "F",
            "G",
            "H",
            "I",
            "J",
            "K",
            "L",
            "M",
            "N",
            "O",
            "P",
            "Q",
            "R",
            "S",
            "T",
            "U",
            "V",
            "W",
            "X",
            "Y",
            "Z"});
            this.drive.Location = new System.Drawing.Point(92, 212);
            this.drive.Name = "drive";
            this.drive.Size = new System.Drawing.Size(53, 20);
            this.drive.TabIndex = 24;
            this.drive.Text = "N";
            // 
            // tabPage2
            // 
            this.tabPage2.Controls.Add(this.withoutOfflineAttribute);
            this.tabPage2.Controls.Add(this.disableCache);
            this.tabPage2.Location = new System.Drawing.Point(4, 21);
            this.tabPage2.Name = "tabPage2";
            this.tabPage2.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage2.Size = new System.Drawing.Size(332, 238);
            this.tabPage2.TabIndex = 1;
            this.tabPage2.Text = "Option";
            this.tabPage2.UseVisualStyleBackColor = true;
            // 
            // disableCache
            // 
            this.disableCache.AutoSize = true;
            this.disableCache.Location = new System.Drawing.Point(6, 16);
            this.disableCache.Name = "disableCache";
            this.disableCache.Size = new System.Drawing.Size(96, 16);
            this.disableCache.TabIndex = 0;
            this.disableCache.Text = "Disable cache";
            this.disableCache.UseVisualStyleBackColor = true;
            // 
            // withoutOfflineAttribute
            // 
            this.withoutOfflineAttribute.AutoSize = true;
            this.withoutOfflineAttribute.Location = new System.Drawing.Point(7, 39);
            this.withoutOfflineAttribute.Name = "withoutOfflineAttribute";
            this.withoutOfflineAttribute.Size = new System.Drawing.Size(145, 16);
            this.withoutOfflineAttribute.TabIndex = 1;
            this.withoutOfflineAttribute.Text = "Without offline attribute";
            this.withoutOfflineAttribute.UseVisualStyleBackColor = true;
            // 
            // label7
            // 
            this.label7.AutoSize = true;
            this.label7.Location = new System.Drawing.Point(17, 10);
            this.label7.Name = "label7";
            this.label7.Size = new System.Drawing.Size(34, 12);
            this.label7.TabIndex = 24;
            this.label7.Text = "Name";
            // 
            // SettingForm
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 12F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(369, 335);
            this.Controls.Add(this.label7);
            this.Controls.Add(this.tabControl1);
            this.Controls.Add(this.settingNames);
            this.Controls.Add(this.cancel);
            this.Controls.Add(this.delete);
            this.Controls.Add(this.connect);
            this.Controls.Add(this.save);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.MaximizeBox = false;
            this.MinimizeBox = false;
            this.Name = "SettingForm";
            this.SizeGripStyle = System.Windows.Forms.SizeGripStyle.Hide;
            this.Text = "SSHFS Setting";
            this.Load += new System.EventHandler(this.SettingForm_Load);
            this.notifyMenu.ResumeLayout(false);
            this.tabControl1.ResumeLayout(false);
            this.tabPage1.ResumeLayout(false);
            this.tabPage1.PerformLayout();
            this.tabPage2.ResumeLayout(false);
            this.tabPage2.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.TextBox host;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.TextBox port;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.TextBox user;
        private System.Windows.Forms.OpenFileDialog openFileDialog1;
        private System.Windows.Forms.TextBox root;
        private System.Windows.Forms.Label label4;
        private System.Windows.Forms.Button connect;
        private System.Windows.Forms.Button cancel;
        private System.Windows.Forms.RadioButton usePassword;
        private System.Windows.Forms.TextBox password;
        private System.Windows.Forms.RadioButton usePrivateKey;
        private System.Windows.Forms.TextBox privatekey;
        private System.Windows.Forms.Button open;
        private System.Windows.Forms.NotifyIcon notifyIcon1;
        private System.Windows.Forms.ContextMenuStrip notifyMenu;
        private System.Windows.Forms.ToolStripMenuItem exit;
        private System.Windows.Forms.Label label5;
        private System.Windows.Forms.ToolStripMenuItem unmount;
        private System.Windows.Forms.Button save;
        private System.Windows.Forms.Button delete;
        private System.Windows.Forms.ComboBox settingNames;
        private System.Windows.Forms.TextBox passphrase;
        private System.Windows.Forms.Label label6;
        private System.Windows.Forms.ToolStripMenuItem mount;
        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPage1;
        private System.Windows.Forms.TabPage tabPage2;
        private System.Windows.Forms.ComboBox drive;
        private System.Windows.Forms.CheckBox withoutOfflineAttribute;
        private System.Windows.Forms.CheckBox disableCache;
        private System.Windows.Forms.Label label7;
    }
}