S3 File System (s3fs) provides an additional file system to your Drupal site,
alongside the public and private file systems, which stores files in Amazon's
Simple Storage Service (S3) (or any S3-compatible storage service). You can set
your site to use S3 File System as the default, or use it only for individual
fields. This functionality is designed for sites which are load-balanced across
multiple servers, as the mechanism used by Drupal's default file systems is not
viable under such a configuration.

=========================================
== Dependencies and Other Requirements ==
=========================================
- Libraries API 2.x - https://drupal.org/project/libraries
- AWS SDK for PHP 2.x - https://github.com/aws/aws-sdk-php/releases
- PHP 5.3.3+ is required. The AWS SDK will not work on earlier versions.
- Your PHP must be configured with "allow_url_fopen = On" in your php.ini file.
  Otherwise, PHP will be unable to open files that are in your S3 bucket.

==================
== Installation ==
==================
1) Install Libraries version 2.x from http://drupal.org/project/libraries.

2) Install the AWS SDK for PHP.
  a) If you have drush, you can install the SDK with this command (executed
    from the root folder of your Drupal codebase):
    drush make --no-core sites/all/modules/s3fs/s3fs.make
  b) If you don't have drush, download the SDK from here:
    https://github.com/aws/aws-sdk-php/releases/download/2.7.25/aws.zip
    Extract that zip file into your Drupal codebase's
    sites/all/libraries/awssdk2 folder such that the path to aws-autoloader.php
    is: sites/all/libraries/awssdk2/aws-autoloader.php

IN CASE OF TROUBLE DETECTING THE AWS SDK LIBRARY:
Ensure that the awssdk2 folder itself, and all the files within it, can be read
by your webserver. Usually this means that the user "apache" (or "_www" on OSX)
must have read permissions for the files, and read+execute permissions for all
the folders in the path leading to the awssdk2 files.

====================
== Initial Setup ==
====================
With the code installation complete, you must now configure s3fs to use your
Amazon Web Services credentials. To do so, store them in the $conf array in
your site's settings.php file (sites/default/settings.php), like so:
$conf['awssdk2_access_key'] = 'YOUR ACCESS KEY';
$conf['awssdk2_secret_key'] = 'YOUR SECRET KEY';

Configure your settings for S3 File System (including your S3 bucket name) at
/admin/config/media/s3fs/settings. You can input your AWS credentials on this
page as well, but using the $conf array is recommended.

You can also configure the rest of your S3 preferences in the $conf array. See
the "Configuring S3FS in settings.php" section below for more info.

===================== ESSENTIAL STEP! DO NOT SKIP THIS! ======================
With the settings saved, go to /admin/config/media/s3fs/actions to refresh the
file metadata cache. This will copy the filenames and attributes for every
existing file in your S3 bucket into Drupal's database. This can take a
significant amount of time for very large buckets (thousands of files). If this
operation times out, you can also perform it using "drush s3fs-refresh-cache".

Please keep in mind that any time the contents of your S3 bucket change without
Drupal knowing about it (like if you copy some files into it manually using
another tool), you'll need to refresh the metadata cache again. S3FS assumes
that its cache is a canonical listing of every file in the bucket. Thus, Drupal
will not be able to access any files you copied into your bucket manually until
S3FS's cache learns of them. This is true of folders as well; s3fs will not be
able to copy files into folders that it doesn't know about.

============================================
== How to Configure Your Site to Use s3fs ==
============================================
Visit the admin/config/media/file-system page and set the "Default download
method" to "Amazon Simple Storage Service"
-and/or-
Add a field of type File, Image, etc. and set the "Upload destination" to
"Amazon Simple Storage Service" in the "Field Settings" tab.

This will configure your site to store new uploaded files in S3. Files which
your site creates automatically (such as aggregated CSS) will still be stored
in the server's local filesystem, because Drupal is hard-coded to use the
public:// filesystem for such files.

However, s3fs can be configured to handle these files, as well. On the s3fs
configuration page (admin/config/media/s3fs) you can enable the "Use S3 for
public:// files" and/or "Use S3 for private:// files" options to make s3fs
take over the job of the public and/or private file systems. This will cause
your site to store newly uploaded/generated files from the public/private file
system in S3 instead of the local file system. However, it will make any
existing files in those file systems become invisible to Drupal. To remedy
this, you'll need to copy those files into your S3 bucket.

You are strongly encouraged to use the drush command "drush s3fs-copy-local"
to do this, as it will copy all the files into the correct subfolders in your
bucket, according to your s3fs configuration, and will write them to the
metadata cache. If you don't have drush, you can use the buttons provided on
the S3FS Actions page (admin/config/media/s3fs/actions), though the copy
operation may fail if you have a lot of files, or very large files. The drush
command will cleanly handle any combination of files.

If you're using nginx rather than Apache, you probably have a config block
like this:

location ~ (^/sites/.*/files/imagecache/|^/sites/default/themes/.*/includes/fonts/|^/sites/.*/files/styles/) {
  expires max;
  try_files $uri @rewrite;
}

To make s3fs's custom image derivative mechanism work, you'll need to modify
that regex it with an additional path, like so:

location ~ (^/s3/files/styles/|^/sites/.*/files/imagecache/|^/sites/default/themes/.*/includes/fonts/|^/sites/.*/files/styles/) {
  expires max;
  try_files $uri @rewrite;
}

=================================
== Aggregated CSS and JS in S3 ==
=================================
If you want your site's aggregated CSS and JS files to be stored on S3, rather
than the default of storing them on the webserver's local filesystem, you'll
need to do two things:
1) Enable the "Use S3 for public:// files" option in the s3fs coniguration,
   because Drupal always* puts aggregated CSS/JS into the public:// filesystem.
2) Because of the way browsers interpret relative URLs used in CSS files, and
   how they restrict requests made from external javascript files, you'll need
   to set up your webserver as a proxy for those files.

* When you've got a module like "Advanced CSS/JS Aggregation" installed, things
get hairy. For now, that module is not compatible with s3fs public:// takeover.

S3FS will present all css files in the taken over public:// filesystem with the
url prefix /s3fs-css/, and all javascript files with /s3fs-js/. So you need to
set up your webserver to proxy those URLs into your S3 bucket.

For Apache, add this code to the right location* in your server's config:

ProxyRequests Off
SSLProxyEngine on
<Proxy *>
    Order deny,allow
    Allow from all
</Proxy>
ProxyPass /s3fs-css/ https://YOUR-BUCKET.s3.amazonaws.com/s3fs-public/
ProxyPassReverse /s3fs-css/ https://YOUR-BUCKET.s3.amazonaws.com/s3fs-public/
ProxyPass /s3fs-js/ https://YOUR-BUCKET.s3.amazonaws.com/s3fs-public/
ProxyPassReverse /s3fs-js/ https://YOUR-BUCKET.s3.amazonaws.com/s3fs-public/

If you're using the "S3FS Root Folder" option, you'll need to insert that
folder before the /s3fs-public/ part of the target URLs. Like so:

ProxyPass /s3fs-css/ https://YOUR-BUCKET.s3.amazonaws.com/YOUR-ROOT-FOLDER/s3fs-public/
ProxyPassReverse /s3fs-css/ https://YOUR-BUCKET.s3.amazonaws.com/YOUR-ROOT-FOLDER/s3fs-public/

* The "right location" is implementation-dependent. Normally, placing these
lines at the bottom of your httpd.conf file should be sufficient. However, if
your site is configured to use SSL, you'll need to put these lines in the
VirtualHost settings for both your normal and SSL sites.


For nginx, add this to your server config:

location ~* ^/(s3fs-css|s3fs-js)/(.*) {
  set $s3_base_path 'YOUR-BUCKET.s3.amazonaws.com/s3fs-public';
  set $file_path $2;

  resolver         172.16.0.23 valid=300s;
  resolver_timeout 10s;

  proxy_pass http://$s3_base_path/$file_path;
}

Again, be sure to take the S3FS Root Folder setting into account, here.

The /s3fs-public/ subfolder is where s3fs stores the files from the public://
filesystem, to avoid name conflicts with files from the s3:// filesystem.

If you're using the "Use a Custom Host" option to store your files in a
non-Amazon file service, you'll need to change the proxy target to the
appropriate URL for your service.

Under some domain name setups, you may be able to avoid the need for proxying
by having the same domain name as your site also point to your S3 bucket. If
that is the case with your site, enable the "Don't rewrite CSS/JS file paths"
option to prevent s3fs from prefixing the URLs for CSS/JS files.

======================================
== Configuring S3FS in settings.php ==
======================================
If you want to configure S3 File System entirely from settings.php, here are
examples of how to configure each setting:

// All the s3fs config settings start with "s3fs_"
$conf['s3fs_bucket'] = 'YOUR BUCKET NAME';
$conf['s3fs_region'] = 'YOUR REGION'';
$conf['s3fs_use_cname'] = TRUE or FALSE;
$conf['s3fs_domain'] = 'cdn.example.com';
$conf['s3fs_use_customhost'] = TRUE or FALSE;
$conf['s3fs_hostname'] = 'host.example.com';
$conf['s3fs_cache_control_header'] = 'public, max-age=300';
$conf['s3fs_encryption'] = 'aws:kms';
$conf['s3fs_use_https'] = TRUE or FALSE;
$conf['s3fs_ignore_cache'] = TRUE or FALSE;
$conf['s3fs_use_s3_for_public'] = TRUE or FALSE;
$conf['s3fs_no_rewrite_cssjs'] = TRUE or FALSE;
$conf['s3fs_use_s3_for_private'] = TRUE or FALSE;
$conf['s3fs_root_folder'] = 'drupal-root';
$conf['s3fs_presigned_urls'] = "300|presigned-files/*\n60|other-presigned/*";
$conf['s3fs_saveas'] = "videos/*\nfull-size-images/*";
$conf['s3fs_torrents'] = "yarrr/*";

// AWS Credentials use a different prefix than the rest of s3fs's settings
$conf['awssdk2_access_key'] = 'YOUR ACCESS KEY';
$conf['awssdk2_secret_key'] = 'YOUR SECRET KEY';
$conf['awssdk2_use_instance_profile'] = TRUE or FALSE;
$conf['awssdk2_default_cache_config'] = '/path/to/cache';


===========================================
== Upgrading from S3 File System 7.x-1.x ==
===========================================
s3fs 7.x-2.x is not 100% backwards-compatible with 7.x-1.x. Most things will
work the same, but if you were using certain options in 1.x, you'll need to
perform some manual intervention to handle the upgrade to 2.x.

The Partial Refresh Prefix setting has been replaced with the Root Folder
setting. Root Folder fulfills the same purpose, but the implementation is
sufficiently different that you'll need to re-configure your site, and
possibly rearrange the files in your S3 bucket to make it work.

With Root Folder, *everything* s3fs does is contained to the specified folder
in your bucket. s3fs acts like the root folder is the bucket root, which means
that the URIs for your files will not reflect the root folder's existence.
Thus, you won't need to configure anything else, like the "file directory"
setting of file and image fields, to make it work.

This is different from how Partial Refresh Prefix worked, because that prefix
*was* reflected in the uris, and you had to configure your file and image
fields appropriately.

So, when upgrading to 7.x-2.x, you'll need to set the Root Folder option to the
same value that you had for Partial Refresh Prefix, and then remove that folder
from your fields' "File directory" settings. Then, move every file that s3fs
previously put into your bucket into the Root Folder. And if there are other
files in your bucket that you want s3fs to know about, move them into there,
too. Then do a metadata refresh.

==================
== Known Issues ==
==================
Some curl libraries, such as the one bundled with MAMP, do not come
with authoritative certificate files. See the following page for details:
http://dev.soup.io/post/56438473/If-youre-using-MAMP-and-doing-something

Because of a bizarre limitation regarding MySQL's maximum index length for
InnoDB tables, the maximum uri length that S3FS supports is 250 characters.
That includes the full path to the file in your bucket, as the full folder
path is part of the uri.

eAccelerator, a deprecated opcode cache plugin for PHP, is incompatible with
AWS SDK for PHP. eAccelerator will corrupt the configuration settings for
the SDK's s3 client object, causing a variety of different exceptions to be
thrown. If your server uses eAccelerator, it is highly recommended that you
replace it with a different opcode cache plugin, as its development was
abandoned several years ago.

=====================
== Acknowledgments ==
=====================
Special recognition goes to justafish, author of the AmazonS3 module:
http://drupal.org/project/amazons3
S3 File System started as a fork of her great module, but has evolved
dramatically since then, becoming a very different beast. The main benefit of
using S3 File System over AmazonS3 is performance, especially for image-
related operations, due to the metadata cache that is central to S3
File System's operation.
