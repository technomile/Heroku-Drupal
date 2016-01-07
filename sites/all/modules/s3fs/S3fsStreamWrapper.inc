<?php

/**
 * @file
 * Drupal stream wrapper implementation for S3 File System.
 *
 * Implements DrupalStreamWrapperInterface to provide an Amazon S3 wrapper
 * using the "s3://" scheme. It can optionally take over for the "public://"
 * stream wrapper, too.
 */

/**
 * The stream wrapper class.
 *
 * In the docs for this class, anywhere you see "<scheme>", it can mean either
 * "s3" or "public", depending on which stream is currently being serviced.
 */
class S3fsStreamWrapper implements DrupalStreamWrapperInterface {

  /**
   * Stream context (this is set by PHP when a context is used).
   *
   * @var resource
   */
  public $context = NULL;

  /**
   * Instance URI referenced as "<scheme>://key".
   *
   * @var string
   */
  protected $uri = NULL;

  /**
   * The AWS SDK for PHP S3Client object.
   *
   * @var Aws\S3\S3Client
   */
  protected $s3 = NULL;

  /**
   * Domain we use to access files over http.
   *
   * @var string
   */
  protected $domain = NULL;

  /**
   * Directory listing used by the dir_* methods.
   *
   * @var array
   */
  protected $dir = NULL;

  /**
   * Map for files that should be delivered with a torrent URL.
   *
   * @var array
   */
  protected $torrents = array();

  /**
   * Files that the user has said must be downloaded, rather than viewed.
   *
   * @var array
   */
  protected $saveas = array();

  /**
   * Files which should be created with URLs that eventually time out.
   *
   * @var array
   */
  protected $presignedURLs = array();

  /**
   * The constructor sets this to TRUE once it's finished.
   *
   * See the comment on _assert_constructor_called() for why this exists.
   *
   * @var bool
   */
  protected $constructed = FALSE;

  /**
   * Default map for determining file mime types.
   *
   * @var array
   */
  protected static $mimeTypeMapping = NULL;

  /**
   * Indicates the current error state in the wrapper.
   *
   * This allows _trigger_error() to tell other stream_* functions to return
   * FALSE when the wrapper encounters an error.
   *
   * @var bool
   */
  protected $_error_state = FALSE;


  /**
   * Stream wrapper constructor.
   *
   * Creates the Aws\S3\S3Client client object and activates the options
   * specified on the S3 File System Settings page.
   */
  public function __construct() {
    // Since S3fsStreamWrapper is always constructed with the same inputs (the
    // file URI is not part of construction), we store the constructed settings
    // statically. This is important for performance because Drupal
    // re-constructs stream wrappers very often.
    $settings = &drupal_static('S3fsStreamWrapper_constructed_settings');
    if ($settings !== NULL) {
      $this->config = $settings['config'];
      $this->s3 = _s3fs_get_amazons3_client($this->config);
      $this->domain = $settings['domain'];
      $this->torrents = $settings['torrents'];
      $this->presignedURLs = $settings['presignedURLs'];
      $this->saveas = $settings['saveas'];
      $this->constructed = TRUE;
      return;
    }
    $this->config = _s3fs_get_config();

    if (empty($this->config['bucket'])) {
      $msg = t('Your AmazonS3 bucket name is not configured. Please visit the !settings_page.',
        array('!settings_page' => l(t('configuration page'), '/admin/config/media/s3fs/settings')));
      watchdog('S3 File System', $msg, array(), WATCHDOG_ERROR);
      throw new Exception($msg);
    }

    // Get the S3 client object.
    $this->s3 = _s3fs_get_amazons3_client($this->config);

    // Always use HTTPS when the page is being served via HTTPS, to avoid
    // complaints from the browser about insecure content.
    global $is_https;
    if ($is_https) {
      // We change the config itself, rather than simply using $is_https in
      // the following if condition, because $this->config['use_https'] gets
      // used again later.
      $this->config['use_https'] = TRUE;
    }

    if (!empty($this->config['use_https'])) {
      $scheme = 'https';
      $this->_debug('Using HTTPS.');
    }
    else {
      $scheme = 'http';
      $this->_debug('Using HTTP.');
    }

    // CNAME support for customizing S3 URLs.
    // If use_cname is not enabled, file URLs do not use $this->domain.
    if (!empty($this->config['use_cname']) && !empty($this->config['domain'])) {
      $domain = check_url($this->config['domain']);
      if ($domain) {
        // If domain is set to a root-relative path, add the hostname back in.
        if (strpos($domain, '/') === 0) {
          $domain = $_SERVER['HTTP_HOST'] . $domain;
        }
        $this->domain = "$scheme://$domain";
      }
      else {
        // Due to the config form's validation, this shouldn't ever happen.
        throw new Exception(t('The "Use a CNAME" option is enabled, but no CDN Domain Name has been set.'));
      }
    }

    // Convert the torrents string to an array.
    if (!empty($this->config['torrents'])) {
      foreach (explode("\n", $this->config['torrents']) as $line) {
        $blob = trim($line);
        if ($blob) {
          $this->torrents[] = $blob;
        }
      }
    }

    // Convert the presigned URLs string to an associative array like
    // array(blob => timeout).
    if (!empty($this->config['presigned_urls'])) {
      foreach (explode("\n", $this->config['presigned_urls']) as $line) {
        $blob = trim($line);
        if ($blob) {
          if (preg_match('/(.*)\|(.*)/', $blob, $matches)) {
            $blob = $matches[2];
            $timeout = $matches[1];
            $this->presignedURLs[$blob] = $timeout;
          }
          else {
            $this->presignedURLs[$blob] = 60;
          }
        }
      }
    }

    // Convert the forced save-as string to an array.
    if (!empty($this->config['saveas'])) {
      foreach (explode("\n", $this->config['saveas']) as $line) {
        $blob = trim($line);
        if ($blob) {
          $this->saveas[] = $blob;
        }
      }
    }

    // Save all the work we just did, so that subsequent S3fsStreamWrapper
    // constructions don't have to repeat it.
    $settings['config'] = $this->config;
    $settings['domain'] = $this->domain;
    $settings['torrents'] = $this->torrents;
    $settings['presignedURLs'] = $this->presignedURLs;
    $settings['saveas'] = $this->saveas;

    $this->constructed = TRUE;
    $this->_debug('S3fsStreamWrapper constructed.');
  }

  /***************************************************************************
                 DrupalStreamWrapperInterface Implementations
  ***************************************************************************/

  /**
   * Static function to determine a file's media type.
   *
   * Uses Drupal's mimetype mapping, unless a different mapping is specified.
   *
   * @return string
   *   The file's MIME type, or 'application/octet-stream' if no type can be
   *   determined.
   */
  public static function getMimeType($uri, $mapping = NULL) {
    self::_debug("getMimeType($uri, $mapping) called.");

    // Load the default mime type map.
    if (!isset(self::$mimeTypeMapping)) {
      include_once DRUPAL_ROOT . '/includes/file.mimetypes.inc';
      self::$mimeTypeMapping = file_mimetype_mapping();
    }

    // If a mapping wasn't specified, use the default map.
    if ($mapping == NULL) {
      $mapping = self::$mimeTypeMapping;
    }

    $extension = '';
    $file_parts = explode('.', basename($uri));

    // Remove the first part: a full filename should not match an extension.
    array_shift($file_parts);

    // Iterate over the file parts, trying to find a match.
    // For my.awesome.image.jpeg, we try:
    // - jpeg
    // - image.jpeg
    // - awesome.image.jpeg
    while ($additional_part = array_pop($file_parts)) {
      $extension = strtolower($additional_part . ($extension ? '.' . $extension : ''));
      if (isset($mapping['extensions'][$extension])) {
        return $mapping['mimetypes'][$mapping['extensions'][$extension]];
      }
    }

    // No mime type matches, so return the default.
    return 'application/octet-stream';
  }

  /**
   * Sets the stream resource URI. URIs are formatted as "<scheme>://filepath".
   *
   * @param string $uri
   *   The URI that should be used for this instance.
   */
  public function setUri($uri) {
    $this->_debug("setUri($uri) called.");

    $this->uri = $uri;
  }

  /**
   * Returns the stream resource URI, which looks like "<scheme>://filepath".
   *
   * @return string
   *   The current URI of the instance.
   */
  public function getUri() {
    $this->_debug("getUri() called for {$this->uri}.");

    return $this->uri;
  }

  /**
   * Returns a web accessible URL for the resource.
   *
   * The format of the returned URL will be different depending on how the S3
   * integration has been configured on the S3 File System admin page.
   *
   * @param bool $no_redirect
   *  A custom parameter for internal use by s3fs.
   *
   * @return string
   *   A web accessible URL for the resource.
   */
  public function getExternalUrl() {
    $this->_debug("getExternalUrl() called for {$this->uri}.");

    // In case we're on Windows, replace backslashes with forward-slashes.
    // Note that $uri is the unaltered value of the File's URI, while
    // $s3_key may be changed at various points to account for implementation
    // details on the S3 side (e.g. root_folder, s3fs-public).
    $s3_key = str_replace('\\', '/', file_uri_target($this->uri));

    // If this is a private:// file, it must be served through the
    // system/files/$path URL, which allows Drupal to restrict access
    // based on who's logged in.
    if (file_uri_scheme($this->uri) == 'private') {
      return url("system/files/$s3_key", array('absolute' => TRUE));
    }

    // When generating an image derivative URL, e.g. styles/thumbnail/blah.jpg,
    // if the file doesn't exist, provide a URL to s3fs's special version of
    // image_style_deliver(), which will create the derivative when that URL
    // gets requested.
    $path_parts = explode('/', $s3_key);
    if ($path_parts[0] == 'styles' && substr($s3_key, -4) != '.css') {
      if (!$this->_s3fs_get_object($this->uri)) {
        // The style delivery path looks like: s3/files/styles/thumbnail/...
        // And $path_parts looks like array('styles', 'thumbnail', ...),
        // so just prepend s3/files/.
        array_unshift($path_parts, 's3', 'files');
        return url(implode('/', $path_parts), array('absolute' => TRUE));
      }
    }

    // Deal with public:// files.
    if (file_uri_scheme($this->uri) == 'public') {
      // Rewrite all css/js file paths unless the user has told us not to.
      if (empty($this->config['no_rewrite_cssjs'])) {
        if (substr($s3_key, -4) == '.css') {
          // Send requests for public CSS files to /s3fs-css/path/to/file.css.
          // Users must set that path up in the webserver config as a proxy into
          // their S3 bucket's s3fs-public/ folder.
          return "{$GLOBALS['base_url']}/s3fs-css/" . drupal_encode_path($s3_key);
        }
        else if (substr($s3_key, -3) == '.js') {
          // Send requests for public JS files to /s3fs-js/path/to/file.js.
          // Like with CSS, the user must set up that path as a proxy.
          return "{$GLOBALS['base_url']}/s3fs-js/" . drupal_encode_path($s3_key);
        }
      }

      // public:// files are stored in S3 inside the s3fs-public/ folder.
      $s3_key = "s3fs-public/$s3_key";
    }

    // Set up the URL settings as speciied in our settings page.
    $url_settings = array(
      'torrent' => FALSE,
      'presigned_url' => FALSE,
      'timeout' => 60,
      'forced_saveas' => FALSE,
      'api_args' => array('Scheme' => !empty($this->config['use_https']) ? 'https' : 'http'),
      'custom_GET_args' => array(),
    );

    // Presigned URLs.
    foreach ($this->presignedURLs as $blob => $timeout) {
      // ^ is used as the delimeter because it's an illegal character in URLs.
      if (preg_match("^$blob^", $s3_key)) {
        $url_settings['presigned_url'] = TRUE;
        $url_settings['timeout'] = $timeout;
        break;
      }
    }
    // Forced Save As.
    foreach ($this->saveas as $blob) {
      if (preg_match("^$blob^", $s3_key)) {
        $filename = basename($s3_key);
        $url_settings['api_args']['ResponseContentDisposition'] = "attachment; filename=\"$filename\"";
        $url_settings['forced_saveas'] = TRUE;
        break;
      }
    }

    // Allow other modules to change the URL settings.
    drupal_alter('s3fs_url_settings', $url_settings, $s3_key);

    // If a root folder has been set, prepend it to the $s3_key at this time.
    if (!empty($this->config['root_folder'])) {
      $s3_key = "{$this->config['root_folder']}/$s3_key";
    }

    if (empty($this->config['use_cname'])) {
      // We're not using a CNAME, so we ask S3 for the URL.
      $expires = NULL;
      if ($url_settings['presigned_url']) {
        $expires = "+{$url_settings['timeout']} seconds";
      }
      else {
        // Due to Amazon's security policies (see Request Parameters section @
        // http://docs.aws.amazon.com/AmazonS3/latest/API/RESTObjectGET.html),
        // only signed requests can use request parameters.
        // Thus, we must provide an expiry time for any URLs which specify
        // Response* API args. Currently, this only includes "Forced Save As".
        foreach ($url_settings['api_args'] as $key => $arg) {
          if (strpos($key, 'Response') === 0) {
            $expires = "+10 years";
            break;
          }
        }
      }
      $external_url = $this->s3->getObjectUrl($this->config['bucket'], $s3_key, $expires, $url_settings['api_args']);
    }
    else {
      // We are using a CNAME, so we need to manually construct the URL.
      $external_url = "{$this->domain}/$s3_key";
    }

    // If this file is versioned, append the version number as a GET arg to
    // ensure that browser caches will be bypassed upon version changes.
    $meta = $this->_read_cache($this->uri);
    if (!empty($meta['version'])) {
      $external_url = $this->_append_get_arg($external_url, $meta['version']);
    }

    // Torrents can only be created for publicly-accessible files:
    // https://forums.aws.amazon.com/thread.jspa?threadID=140949
    // So Forced SaveAs and Presigned URLs cannot be served as torrents.
    if (!$url_settings['forced_saveas'] && !$url_settings['presigned_url']) {
      foreach ($this->torrents as $blob) {
        if (preg_match("^$blob^", $s3_key)) {
          // You get a torrent URL by adding a "torrent" GET arg.
          $external_url = $this->_append_get_arg($external_url, 'torrent');
          break;
        }
      }
    }

    // If another module added a 'custom_GET_args' array to the url settings, process it here.
    if (!empty($url_settings['custom_GET_args'])) {
      foreach ($url_settings['custom_GET_args'] as $name => $value) {
        $external_url = $this->_append_get_arg($external_url, $name, $value);
      }
    }

    return $external_url;
  }

  /**
   * Gets the path that the wrapper is responsible for.
   *
   * This function isn't part of DrupalStreamWrapperInterface, but the rest
   * of Drupal calls it as if it were, so we need to define it.
   *
   * @return string
   *   The empty string. Since this is a remote stream wrapper,
   *   it has no directory path.
   */
  public function getDirectoryPath() {
    $this->_debug("getDirectoryPath() called.");

    return '';
  }

  /***************************************************************************
              Public Functions for External Use of the Wrapper
  ***************************************************************************/

  /**
   * Wait for the specified file to exist in the bucket.
   *
   * @param string $uri
   *   The URI of the file.
   *
   * @return bool
   *   Returns TRUE once the waiting finishes, or FALSE if the file does not
   *   begin to exist within 10 seconds.
   */
  public function waitUntilFileExists($uri) {
    $wait_params = $this->_get_params($uri);
    // Retry ten times, once every second.
    $wait_params['waiter.max_attempts'] = 10;
    $wait_params['waiter.interval'] = 1;
    try {
      $this->s3->waitUntilObjectExists($wait_params);
    }
    catch (Aws\Common\Exception\RuntimeException $e) {
      return FALSE;
    }
    return TRUE;
  }

  /**
   * Write the file at the given URI into the metadata cache.
   *
   * This function is public so that other code can upload files to S3 and
   * then have us write the correct metadata into our cache.
   */
  public function writeUriToCache($uri) {
    if (!$this->waitUntilFileExists($uri)) {
      throw new S3fsException(t('The file at URI %file does not exist in S3.', array('%file' => $uri)));
    }
    $metadata = $this->_get_metadata_from_s3($uri);
    $this->_write_cache($metadata);
    clearstatcache(TRUE, $uri);
  }

  /***************************************************************************
                      PHP Stream Wrapper Implementations
  ***************************************************************************/

  /**
   * This wrapper doesn't support file permissions.
   *
   * @param int $mode
   *   The file's desired permissions in octal. Consult PHP chmod() documentation
   *   for more information.
   *
   * @return bool
   *   Always returns TRUE.
   */
  public function chmod($mode) {
    $this->_assert_constructor_called();
    $octal_mode = decoct($mode);
    $this->_debug("chmod($octal_mode) called. S3fsStreamWrapper does not support this function.");

    return TRUE;
  }

  /**
   * This wrapper does not support realpath().
   *
   * @return bool
   *   Always returns FALSE.
   */
  public function realpath() {
    $this->_debug("realpath() called for {$this->uri}. S3fsStreamWrapper does not support this function.");

    return FALSE;
  }

  /**
   * Gets the name of the parent directory of a given path.
   *
   * This method is usually accessed through drupal_dirname(), which wraps
   * around the normal PHP dirname() function, since it doesn't support stream
   * wrappers.
   *
   * @param string $uri
   *   An optional URI.
   *
   * @return string
   *   The directory name, or FALSE if not applicable.
   *
   * @see drupal_dirname()
   */
  public function dirname($uri = NULL) {
    $this->_debug("dirname($uri) called.");

    if (!isset($uri)) {
      $uri = $this->uri;
    }
    $scheme = file_uri_scheme($uri);
    $dirname = dirname(file_uri_target($uri));

    // When the dirname() call above is given '$scheme://', it returns '.'.
    // But '$scheme://.' is an invalid uri, so we return "$scheme://" instead.
    if ($dirname == '.') {
      $dirname = '';
    }

    return "$scheme://$dirname";
  }

  /**
   * Support for fopen(), file_get_contents(), file_put_contents() etc.
   *
   * @param string $uri
   *   The URI of the file to open.
   * @param string $mode
   *   The file mode. Only 'r', 'w', 'a', and 'x' are supported.
   * @param int $options
   *   A bit mask of STREAM_USE_PATH and STREAM_REPORT_ERRORS.
   * @param string $opened_path
   *   An OUT parameter populated with the path which was opened.
   *   This wrapper does not support this parameter.
   *
   * @return bool
   *   TRUE if file was opened successfully. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-open.php
   */
  public function stream_open($uri, $mode, $options, &$opened_path) {
    $this->_debug("stream_open($uri, $mode, $options, $opened_path) called.");

    $this->uri = $uri;
    // We don't care about the binary flag, so strip it out.
    $this->access_mode = $mode = rtrim($mode, 'bt');
    $this->params = $this->_get_params($uri);
    $errors = array();

    if (strpos($mode, '+')) {
      $errors[] = t('The S3 File System stream wrapper does not allow simultaneous reading and writing.');
    }
    if (!in_array($mode, array('r', 'w', 'a', 'x'))) {
      $errors[] = t("Mode not supported: %mode. Use one 'r', 'w', 'a', or 'x'.", array('%mode' => $mode));
    }
    // When using mode "x", validate if the file exists first.
    if ($mode == 'x' && $this->_read_cache($uri)) {
      $errors[] = t("%uri already exists in your S3 bucket, so it cannot be opened with mode 'x'.", array('%uri' => $uri));
    }

    if (!$errors) {
      if ($mode == 'r') {
        $this->_open_read_stream($this->params, $errors);
      }
      else if ($mode == 'a') {
        $this->_open_append_stream($this->params, $errors);
      }
      else {
        $this->_open_write_stream($this->params, $errors);
      }
    }

    return $errors ? $this->_trigger_error($errors) : TRUE;
  }

  /**
   * Support for fclose().
   *
   * Clears the object buffer.
   *
   * @return bool
   *   Always returns TRUE.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-close.php
   */
  public function stream_close() {
    $this->_debug("stream_close() called for {$this->uri}.");

    $this->body = NULL;
    $this->params = NULL;
    return $this->_error_state;
  }

  /**
   * This wrapper does not support flock().
   *
   * @return bool
   *   Always Returns FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-lock.php
   */
  public function stream_lock($operation) {
    $this->_debug("stream_lock($operation) called. S3fsStreamWrapper doesn't support this function.");

    return FALSE;
  }

  /**
   * Support for fread(), file_get_contents() etc.
   *
   * @param int $count
   *   Maximum number of bytes to be read.
   *
   * @return string
   *   The data which was read from the stream, or FALSE in case of an error.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-read.php
   */
  public function stream_read($count) {
    $this->_debug("stream_read($count) called for {$this->uri}.");

    return $this->body->read($count);
  }

  /**
   * Support for fwrite(), file_put_contents() etc.
   *
   * @param string $data
   *   The data to be written to the stream.
   *
   * @return int
   *   The number of bytes actually written to the stream.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-write.php
   */
  public function stream_write($data) {
    $bytes = strlen($data);
    $this->_debug("stream_write() called with $bytes bytes of data for {$this->uri}.");

    return $this->body->write($data);
  }

  /**
   * Support for feof().
   *
   * @return bool
   *   TRUE if end-of-file has been reached. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-eof.php
   */
  public function stream_eof() {
    $this->_debug("stream_eof() called for {$this->uri}.");

    return $this->body->feof();
  }

  /**
   * Support for fseek().
   *
   * @param int $offset
   *   The byte offset to got to.
   * @param int $whence
   *   SEEK_SET, SEEK_CUR, or SEEK_END.
   *
   * @return bool
   *   TRUE on success. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-seek.php
   */
  public function stream_seek($offset, $whence) {
    $this->_debug("stream_seek($offset, $whence) called.");

    return $this->body->seek($offset, $whence);
  }

  /**
   * Support for fflush(). Flush current cached stream data to a file in S3.
   *
   * @return bool
   *   TRUE if data was successfully stored in S3.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-flush.php
   */
  public function stream_flush() {
    $this->_debug("stream_flush() called for {$this->uri}.");

    if ($this->access_mode == 'r') {
      return FALSE;
    }

    // Prep the upload parameters.
    $this->body->rewind();
    $upload_params = $this->params;
    $upload_params['Body'] = $this->body;
    $upload_params['ContentType'] = S3fsStreamWrapper::getMimeType($this->uri);
    if (file_uri_scheme($this->uri) != 'private') {
      // All non-private files uploaded to S3 must be set to public-read, or users' browsers
      // will get PermissionDenied errors, and torrent URLs won't work.
      $upload_params['ACL'] = 'public-read';
    }
    // Set the Cache-Control header, if the user specified one.
    if (!empty($this->config['cache_control_header'])) {
      $upload_params['CacheControl'] = $this->config['cache_control_header'];
    }

    if (!empty($this->config['encryption'])) {
      $upload_params['ServerSideEncryption'] = $this->config['encryption'];
    }

    // Allow other modules to alter the upload params.
    drupal_alter('s3fs_upload_params', $upload_params);

    try {
      $this->s3->putObject($upload_params);
      $this->writeUriToCache($this->uri);
    }
    catch (\Exception $e) {
      $this->_debug($e->getMessage());
      return $this->_trigger_error($e->getMessage());
    }
    return TRUE;
  }

  /**
   * Support for ftell().
   *
   * @return int
   *   The current offset in bytes from the beginning of file.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-tell.php
   */
  public function stream_tell() {
    $this->_debug("stream_tell() called.");

    return $this->body->ftell();
  }

  /**
   * Support for fstat().
   *
   * @return array
   *   An array with file status, or FALSE in case of an error.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-stat.php
   */
  public function stream_stat() {
    $this->_debug("stream_stat() called for {$this->uri}.");

    $stat = fstat($this->body->getStream());
    // Add the size of the underlying stream if it is known.
    if ($this->access_mode == 'r' && $this->body->getSize()) {
      $stat[7] = $stat['size'] = $this->body->getSize();
    }

    return $stat;
  }

  /**
   * Cast the stream to return the underlying file resource
   *
   * @param int $cast_as
   *   STREAM_CAST_FOR_SELECT or STREAM_CAST_AS_STREAM
   *
   * @return resource
   */
  public function stream_cast($cast_as) {
    $this->_debug("stream_cast($cast_as) called.");

    return $this->body->getStream();
  }

  /**
   * Support for unlink().
   *
   * @param string $uri
   *   The uri of the resource to delete.
   *
   * @return bool
   *   TRUE if resource was successfully deleted, regardless of whether or not
   *   the file actually existed.
   *   FALSE if the call to S3 failed, in which case the file will not be
   *   removed from the cache.
   *
   * @see http://php.net/manual/en/streamwrapper.unlink.php
   */
  public function unlink($uri) {
    $this->_assert_constructor_called();
    $this->_debug("unlink($uri) called.");

    try {
      $this->s3->deleteObject($this->_get_params($uri));
      $this->_delete_cache($uri);
      clearstatcache(TRUE, $uri);
    }
    catch (\Exception $e) {
      $this->_debug($e->getMessage());
      return $this->_trigger_error($e->getMessage());
    }
    return TRUE;
  }

  /**
   * Support for rename().
   *
   * If $to_uri exists, this file will be overwritten. This behavior is
   * identical to the PHP rename() function.
   *
   * @param string $from_uri
   *   The uri of the file to be renamed.
   * @param string $to_uri
   *   The new uri for the file.
   *
   * @return bool
   *   TRUE if file was successfully renamed. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.rename.php
   */
  public function rename($from_uri, $to_uri) {
    $this->_assert_constructor_called();
    $this->_debug("rename($from_uri, $to_uri) called.");

    $from_params = $this->_get_params($from_uri);
    $to_params = $this->_get_params($to_uri);
    clearstatcache(TRUE, $from_uri);
    clearstatcache(TRUE, $to_uri);

    // Add the copyObject() parameters.
    $to_params['CopySource'] = "/{$from_params['Bucket']}/" . rawurlencode($from_params['Key']);
    $to_params['MetadataDirective'] = 'COPY';
    if (file_uri_scheme($from_uri) != 'private') {
      $to_params['ACL'] = 'public-read';
    }

    try {
      // Copy the original object to the specified destination.
      $this->s3->copyObject($to_params);
      // Copy the original object's metadata.
      $metadata = $this->_read_cache($from_uri);
      $metadata['uri'] = $to_uri;
      $this->_write_cache($metadata);
      $this->waitUntilFileExists($to_uri);
      // Now that we know the new object is there, delete the old one.
      return $this->unlink($from_uri);
    }
    catch (\Exception $e) {
      $this->_debug($e->getMessage());
      return $this->_trigger_error($e->getMessage());
    }
  }

  /**
   * Support for mkdir().
   *
   * @param string $uri
   *   The URI to the directory to create.
   * @param int $mode
   *   Permission flags - see mkdir().
   * @param int $options
   *   A bit mask of STREAM_REPORT_ERRORS and STREAM_MKDIR_RECURSIVE.
   *
   * @return bool
   *   TRUE if the directory was successfully created. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.mkdir.php
   */
  public function mkdir($uri, $mode, $options) {
    $this->_assert_constructor_called();
    $this->_debug("mkdir($uri, $mode, $options) called.");

    clearstatcache(TRUE, $uri);
    // If this URI already exists in the cache, return TRUE if it's a folder
    // (so that recursive calls won't improperly report failure when they
    // reach an existing ancestor), or FALSE if it's a file (failure).
    $test_metadata = $this->_read_cache($uri);
    if ($test_metadata) {
      return (bool) $test_metadata['dir'];
    }

    // S3 is a flat file system, with no concept of directories (just files
    // with slashes in their names). We store folders in the metadata cache,
    // but don't create an object for them in S3.
    $metadata = _s3fs_convert_metadata($uri, array());
    $this->_write_cache($metadata);

    // If the STREAM_MKDIR_RECURSIVE option was specified, also create all the
    // ancestor folders of this uri, except for the root directory.
    $parent_dir = drupal_dirname($uri);
    if (($options & STREAM_MKDIR_RECURSIVE) && file_uri_target($parent_dir) != '') {
      return $this->mkdir($parent_dir, $mode, $options);
    }
    return TRUE;
  }

  /**
   * Support for rmdir().
   *
   * @param string $uri
   *   The URI to the folder to delete.
   * @param int $options
   *   A bit mask of STREAM_REPORT_ERRORS.
   *
   * @return bool
   *   TRUE if folder is successfully removed.
   *   FALSE if $uri isn't a folder, or the folder is not empty.
   *
   * @see http://php.net/manual/en/streamwrapper.rmdir.php
   */
  public function rmdir($uri, $options) {
    $this->_assert_constructor_called();
    $this->_debug("rmdir($uri, $options) called.");

    if (!$this->_uri_is_dir($uri)) {
      return FALSE;
    }

    // We need a version of $uri with no / because folders are cached with no /.
    // We also need one with the /, because it might be a file in S3 that
    // ends with /. In addition, we must differentiate against files with this
    // folder's name as a substring.
    // e.g. rmdir('s3://foo/bar') should ignore s3://foo/barbell.jpg.
    $bare_uri = rtrim($uri, '/');
    $slash_uri = $bare_uri . '/';

    // Check if the folder is empty.
    $files = db_select('s3fs_file', 's')
      ->fields('s')
      ->condition('uri', db_like($slash_uri) . '%', 'LIKE')
      ->execute()
      ->fetchAll(PDO::FETCH_ASSOC);

    // If the folder is empty, it's eligible for deletion.
    if (empty($files)) {
      $result = $this->_delete_cache($bare_uri);
      clearstatcache(TRUE, $bare_uri);

      // Also delete the object from S3, if it's there.
      $params = $this->_get_params($slash_uri);
      try {
        if ($this->s3->doesObjectExist($params['Bucket'], $params['Key'])) {
          $this->s3->deleteObject($params);
        }
      }
      catch (\Exception $e) {
        $this->_debug($e->getMessage());
        return $this->_trigger_error($e->getMessage());
      }

      return (bool) $result;
    }

    // The folder is non-empty.
    return FALSE;
  }

  /**
   * Support for stat().
   *
   * @param string $uri
   *   The URI to get information about.
   * @param int $flags
   *   A bit mask of STREAM_URL_STAT_LINK and STREAM_URL_STAT_QUIET.
   *   S3fsStreamWrapper ignores this value.
   *
   * @return array
   *   An array with file status, or FALSE in case of an error.
   *
   * @see http://php.net/manual/en/streamwrapper.url-stat.php
   */
  public function url_stat($uri, $flags) {
    $this->_assert_constructor_called();
    $this->_debug("url_stat($uri, $flags) called.");

    return $this->_stat($uri);
  }

  /**
   * Support for opendir().
   *
   * @param string $uri
   *   The URI to the directory to open.
   * @param int $options
   *   A flag used to enable safe_mode.
   *   This wrapper doesn't support safe_mode, so this parameter is ignored.
   *
   * @return bool
   *   TRUE on success. Otherwise, FALSE.
   *
   * @see http://php.net/manual/en/streamwrapper.dir-opendir.php
   */
  public function dir_opendir($uri, $options = NULL) {
    $this->_assert_constructor_called();
    $this->_debug("dir_opendir($uri, $options) called.");

    if (!$this->_uri_is_dir($uri)) {
      return FALSE;
    }

    $scheme = file_uri_scheme($uri);
    $bare_uri = rtrim($uri, '/');
    $slash_uri = $bare_uri . '/';

    // If this URI was originally a root folder (e.g. s3://), the above code
    // removed *both* slashes but only added one back. So we need to add
    // back the second slash.
    if ($slash_uri == "$scheme:/") {
      $slash_uri = "$scheme://";
    }

    // Get the list of uris for files and folders which are children of the
    // specified folder, but not grandchildren.
    $child_uris = db_select('s3fs_file', 's')
      ->fields('s', array('uri'))
      ->condition('uri', db_like($slash_uri) . '%', 'LIKE')
      ->condition('uri', db_like($slash_uri) . '%/%', 'NOT LIKE')
      ->execute()
      ->fetchCol(0);

    $this->dir = array();
    foreach ($child_uris as $child_uri) {
      $this->dir[] = basename($child_uri);
    }
    return TRUE;
  }

  /**
   * Support for readdir().
   *
   * @return string
   *   The next filename, or FALSE if there are no more files in the directory.
   *
   * @see http://php.net/manual/en/streamwrapper.dir-readdir.php
   */
  public function dir_readdir() {
    $this->_debug("dir_readdir() called.");

    $entry = each($this->dir);
    return $entry ? $entry['value'] : FALSE;
  }

  /**
   * Support for rewinddir().
   *
   * @return bool
   *   Always returns TRUE.
   *
   * @see http://php.net/manual/en/streamwrapper.dir-rewinddir.php
   */
  public function dir_rewinddir() {
    $this->_debug("dir_rewinddir() called.");

    reset($this->dir);
    return TRUE;
  }

  /**
   * Support for closedir().
   *
   * @return bool
   *   Always returns TRUE.
   *
   * @see http://php.net/manual/en/streamwrapper.dir-closedir.php
   */
  public function dir_closedir() {
    $this->_debug("dir_closedir() called.");

    unset($this->dir);
    return TRUE;
  }

  /***************************************************************************
                              Internal Functions
  ***************************************************************************/

  /**
   * Get the status of the file with the specified URI.
   *
   * @return array
   *   An array with file status, or FALSE if the file doesn't exist.
   *
   * @see http://php.net/manual/en/streamwrapper.stream-stat.php
   */
  protected function _stat($uri) {
    $this->_debug("_stat($uri) called.", TRUE);

    $metadata = $this->_s3fs_get_object($uri);
    if ($metadata) {
      $stat = array();
      $stat[0]  = $stat['dev'] = 0;
      $stat[1]  = $stat['ino'] = 0;
      // Use the S_IFDIR posix flag for directories, S_IFREG for files.
      // All files are considered writable, so OR in 0777.
      $stat[2]  = $stat['mode'] = ($metadata['dir'] ? 0040000 : 0100000) | 0777;
      $stat[3]  = $stat['nlink'] = 0;
      $stat[4]  = $stat['uid'] = 0;
      $stat[5]  = $stat['gid'] = 0;
      $stat[6]  = $stat['rdev'] = 0;
      $stat[7]  = $stat['size'] = 0;
      $stat[8]  = $stat['atime'] = 0;
      $stat[9]  = $stat['mtime'] = 0;
      $stat[10] = $stat['ctime'] = 0;
      $stat[11] = $stat['blksize'] = 0;
      $stat[12] = $stat['blocks'] = 0;

      if (!$metadata['dir']) {
        $stat[4]  = $stat['uid'] = 's3fs';
        $stat[7]  = $stat['size'] = $metadata['filesize'];
        $stat[8]  = $stat['atime'] = $metadata['timestamp'];
        $stat[9]  = $stat['mtime'] = $metadata['timestamp'];
        $stat[10] = $stat['ctime'] = $metadata['timestamp'];
      }
      return $stat;
    }
    return FALSE;
  }

  /**
   * Determine whether the $uri is a directory.
   *
   * @param string $uri
   *   The uri of the resource to check.
   *
   * @return bool
   *   TRUE if the resource is a directory.
   */
  protected function _uri_is_dir($uri) {
    $metadata = $this->_s3fs_get_object($uri);
    return $metadata ? $metadata['dir'] : FALSE;
  }

  /**
   * Try to fetch an object from the metadata cache.
   *
   * If that file isn't in the cache, we assume it doesn't exist.
   *
   * @param string $uri
   *   The uri of the resource to check.
   *
   * @return bool
   *   An array if the $uri exists, otherwise FALSE.
   */
  protected function _s3fs_get_object($uri) {
    $this->_debug("_s3fs_get_object($uri) called.", TRUE);

    // For the root directory, return metadata for a generic folder.
    if (file_uri_target($uri) == '') {
      return _s3fs_convert_metadata('/', array());
    }

    // Trim any trailing '/', in case this is a folder request.
    $uri = rtrim($uri, '/');

    // Check if this URI is in the cache.
    $metadata = $this->_read_cache($uri);

    // If cache ignore is enabled, query S3 for all URIs which aren't in the
    // cache, and non-folder URIs which are.
    if (!empty($this->config['ignore_cache']) && !$metadata['dir']) {
      try {
        // If _get_metadata_from_s3() returns FALSE, the file doesn't exist.
        $metadata = $this->_get_metadata_from_s3($uri);
      }
      catch (\Exception $e) {
        $this->_debug($e->getMessage());
        return $this->_trigger_error($e->getMessage());
      }
    }

    return $metadata;
  }

  /**
   * Fetch an object from the file metadata cache table.
   *
   * @param string $uri
   *   The uri of the resource to check.
   *
   * @return array
   *   An array of metadata if the $uri is in the cache. Otherwise, FALSE.
   */
  protected function _read_cache($uri) {
    $this->_debug("_read_cache($uri) called.", TRUE);

    // Since public:///blah.jpg and public://blah.jpg refer to the same file
    // (a file named blah.jpg at the root of the file system), we'll sometimes
    // receive files with a /// in their URI. This messes with our caching
    // scheme, though, so we need to remove the extra /.
    if (strpos($uri, 'public:///') === 0) {
      $uri = preg_replace('^public://[/]+^', 'public://', $uri);
    }
    else if (strpos($uri, 'private:///') === 0) {
      $uri = preg_replace('^private://[/]+^', 'private://', $uri);
    }

    // Cache DB reads so that faster caching mechanisms (e.g. redis, memcache)
    // can further improve performance.
    $cid = S3FS_CACHE_PREFIX . $uri;
    if ($cached = cache_get($cid, S3FS_CACHE_BIN)) {
      $record = $cached->data;
    }
    else {
      // Cache miss. Avoid a stampede.
      if (!lock_acquire($cid, 1)) {
        // Another request is building the variable cache. Wait, then re-run
        // this function.
        lock_wait($cid);
        $record = $this->_read_cache($uri);
      }
      else {
        $record = db_select('s3fs_file', 's')
          ->fields('s')
          ->condition('uri', $uri, '=')
          ->execute()
          ->fetchAssoc();

        cache_set($cid, $record, S3FS_CACHE_BIN, CACHE_TEMPORARY);
        lock_release($cid);
      }
    }

    return $record ? $record : FALSE;
  }

  /**
   * Write an object's (and its ancestor folders') metadata to the cache.
   *
   * @param array $metadata
   *   An associative array of file metadata in this format:
   *     'uri' => The full URI of the file, including the scheme.
   *     'filesize' => The size of the file, in bytes.
   *     'timestamp' => The file's create/update timestamp.
   *     'dir' => A boolean indicating whether the object is a directory.
   *
   * @throws
   *   Exceptions which occur in the database call will percolate.
   */
  protected function _write_cache($metadata) {
    $this->_debug("_write_cache({$metadata['uri']}) called.", TRUE);

    // Since public:///blah.jpg and public://blah.jpg refer to the same file
    // (a file named blah.jpg at the root of the file system), we'll sometimes
    // receive files with a /// in their URI. This messes with our caching
    // scheme, though, so we need to remove the extra /.
    if (strpos($metadata['uri'], 'public:///') === 0) {
      $metadata['uri'] = preg_replace('^public://[/]+^', 'public://', $metadata['uri']);
    }
    else if (strpos($metadata['uri'], 'private:///') === 0) {
      $metadata['uri'] = preg_replace('^private://[/]+^', 'private://', $metadata['uri']);
    }

    db_merge('s3fs_file')
      ->key(array('uri' => $metadata['uri']))
      ->fields($metadata)
      ->execute();

    // Clear this URI from the Drupal cache, to ensure the next read isn't
    // from a stale cache entry.
    $cid = S3FS_CACHE_PREFIX . $metadata['uri'];
    cache_clear_all($cid, S3FS_CACHE_BIN);

    $dirname = drupal_dirname($metadata['uri']);
    // If this file isn't in the root directory, also write this file's
    // ancestor folders to the cache.
    if (file_uri_target($dirname) != '') {
      $this->mkdir($dirname, NULL, STREAM_MKDIR_RECURSIVE);
    }
  }

  /**
   * Delete an object's metadata from the cache.
   *
   * @param mixed $uri
   *   A string (or array of strings) containing the URI(s) of the object(s)
   *   to be deleted.
   *
   * @throws
   *   Exceptions which occur in the database call will percolate.
   */
  protected function _delete_cache($uri) {
    $this->_debug("_delete_cache($uri) called.", TRUE);

    if (!is_array($uri)) {
      $uri = array($uri);
    }

    // Build an OR query to delete all the URIs at once.
    $delete_query = db_delete('s3fs_file');
    $or = db_or();
    foreach ($uri as $u) {
      $or->condition('uri', $u, '=');
      // Clear this URI from the Drupal cache.
      $cid = S3FS_CACHE_PREFIX . $u;
      cache_clear_all($cid, S3FS_CACHE_BIN);
    }
    $delete_query->condition($or);
    return $delete_query->execute();
  }

  /**
   * Get the stream context options available to the current stream.
   *
   * @return array
   */
  protected function _get_options() {
    $context = isset($this->context) ? $this->context : stream_context_get_default();
    $options = stream_context_get_options($context);
    return isset($options['s3']) ? $options['s3'] : array();
  }

  /**
   * Get a specific stream context option.
   *
   * @param string $name
   *   Name of the option to retrieve.
   *
   * @return mixed|null
   */
  protected function _get_option($name) {
    $options = $this->_get_options();
    return isset($options[$name]) ? $options[$name] : NULL;
  }

  /**
   * Get the Command parameters for the specified URI.
   *
   * @param string $uri
   *   The URI of the file.
   *
   * @return array
   *   A Command parameters array, including 'Bucket', 'Key', and
   *   context parameters.
   */
  protected function _get_params($uri) {
    $params = $this->_get_options();
    unset($params['seekable']);
    unset($params['throw_exceptions']);

    $params['Bucket'] = $this->config['bucket'];
    $params['Key'] = file_uri_target($uri);

    // public:// file are all placed in the s3fs-public/ folder.
    if (file_uri_scheme($uri) == 'public') {
      $params['Key'] = "s3fs-public/{$params['Key']}";
    }
    // private:// file are all placed in the s3fs-private/ folder.
    else if (file_uri_scheme($uri) == 'private') {
      $params['Key'] = "s3fs-private/{$params['Key']}";
    }

    // If it's set, all files are placed in the root folder.
    if (!empty($this->config['root_folder'])) {
      $params['Key'] = "{$this->config['root_folder']}/{$params['Key']}";
    }

    return $params;
  }

  /**
   * Initialize the stream wrapper for a read only stream.
   *
   * @param array $params
   *   An array of AWS SDK for PHP Command parameters.
   * @param array $errors
   *   Array to which encountered errors should be appended.
   */
  protected function _open_read_stream($params, &$errors) {
    $this->_debug("_open_read_stream({$params['Key']}) called.", TRUE);

    // Create the command and serialize the request.
    $request = $this->_get_signed_request($this->s3->getCommand('GetObject', $params));
    // Create a stream that uses the EntityBody object.
    $factory = $this->_get_option('stream_factory');
    if (empty($factory)) {
      $factory = new Guzzle\Stream\PhpStreamRequestFactory();
    }
    $this->body = $factory->fromRequest($request, array(), array('stream_class' => 'Guzzle\Http\EntityBody'));

    // Wrap the body in an S3fsSeekableCachingEntityBody, so that seeks can
    // go to not-yet-read sections of the file.
    if (class_exists('S3fsSeekableCachingEntityBody')) {
      $this->body = new S3fsSeekableCachingEntityBody($this->body);
    }
  }

  /**
   * Initialize the stream wrapper for an append stream.
   *
   * @param array $params
   *   An array of AWS SDK for PHP Command parameters.
   * @param array $errors
   *   OUT parameter: all encountered errors are appended to this array.
   */
  protected function _open_append_stream($params, &$errors) {
    $this->_debug("_open_append_stream({$params['Key']}) called.", TRUE);

    try {
      // Get the body of the object
      $this->body = $this->s3->getObject($params)->get('Body');
      $this->body->seek(0, SEEK_END);
    }
    catch (Aws\S3\Exception\S3Exception $e) {
      // The object does not exist, so use a simple write stream.
      $this->_open_write_stream($params, $errors);
    }
  }

  /**
   * Initialize the stream wrapper for a write only stream.
   *
   * @param array $params
   *   An array of AWS SDK for PHP Command parameters.
   * @param array $errors
   *   OUT parameter: all encountered errors are appended to this array.
   */
  protected function _open_write_stream($params, &$errors) {
    $this->_debug("_open_write_stream({$params['Key']}) called.", TRUE);

    $this->body = new Guzzle\Http\EntityBody(fopen('php://temp', 'r+'));
  }

  /**
   * Serialize and sign a command, returning a request object.
   *
   * @param CommandInterface $command
   *   The Command to sign.
   *
   * @return RequestInterface
   */
  protected function _get_signed_request($command) {
    $this->_debug("_get_signed_request() called.", TRUE);

    $request = $command->prepare();
    $request->dispatch('request.before_send', array('request' => $request));
    return $request;
  }

  /**
   * Returns the converted metadata for an object in S3.
   *
   * @param string $uri
   *   The URI for the object in S3.
   *
   * @return array
   *   An array of DB-compatible file metadata.
   *
   * @throws \Exception
   *   Any exception raised by the listObjects() S3 command will percolate
   *   out of this function.
   */
  protected function _get_metadata_from_s3($uri) {
    $this->_debug("_get_metadata_from_s3($uri) called.", TRUE);

    $params = $this->_get_params($uri);
    try {
      $result = $this->s3->headObject($params);
    }
    catch (Aws\S3\Exception\NoSuchKeyException $e) {
      // headObject() throws this exception if the requested key doesn't exist
      // in the bucket.
      return FALSE;
    }

    return _s3fs_convert_metadata($uri, $result);
  }

  /**
   * Triggers one or more errors.
   *
   * @param string|array $errors
   *   Errors to trigger.
   * @param mixed $flags
   *   If set to STREAM_URL_STAT_QUIET, no error or exception is triggered.
   *
   * @return bool
   *   Always returns FALSE.
   *
   * @throws RuntimeException
   *   If the 'throw_exceptions' option is TRUE.
   */
  protected function _trigger_error($errors, $flags = NULL) {
    if ($flags != STREAM_URL_STAT_QUIET) {
      if ($this->_get_option('throw_exceptions')) {
        throw new RuntimeException(implode("\n", (array) $errors));
      }
      else {
        trigger_error(implode("\n", (array) $errors), E_USER_ERROR);
      }
    }
    $this->_error_state = TRUE;
    return FALSE;
  }

  /**
   * Call the constructor it it hasn't been called yet.
   *
   * Due to PHP bug #40459, the constructor of this class isn't always called
   * for some of the methods.
   *
   * @see https://bugs.php.net/bug.php?id=40459
   */
  protected function _assert_constructor_called() {
    if (!$this->constructed) {
      $this->__construct();
    }
  }

  /**
   * Logging function used for debugging.
   *
   * This function only writes anything if the global variable $_s3fs_debug
   * is TRUE.
   *
   * @param string $msg
   *   The debug message to log.
   * @param bool $internal
   *   If this is TRUE, don't log $msg unless $_s3fs_debug_internal is TRUE.
   */
  protected static function _debug($msg, $internal = FALSE) {
    global $_s3fs_debug, $_s3fs_debug_internal;
    if ($_s3fs_debug && (!$internal || $_s3fs_debug_internal)) {
      debug($msg);
    }
  }

  /**
   * Helper function to safely append a GET argument to a given base URL.
   *
   * @param string $base_url
   *   The URL onto which the GET arg will be appended.
   * @param string $name
   *   The name of the GET argument.
   * @param string $value
   *   The value of the GET argument. Optional.
   */
  protected static function _append_get_arg($base_url, $name, $value = NULL) {
    $separator = strpos($base_url, '?') === FALSE ? '?' : '&';
    $new_url = "{$base_url}{$separator}{$name}";
    if ($value !== NULL) {
      $new_url .= "=$value";
    }
    return $new_url;
  }
}

// Guzzle\Http\CachingEntityBody is only defined once the SDK has been loaded,
// so we need to load it before we can inherit from it.
$library = _s3fs_load_awssdk2_library();
if ($library['loaded']) {
  /**
   * A replacement class for CachingEntityBody that serves better for s3fs.
   *
   * Any instantiation of this class must be wrapped in a check for its
   * existence, since it may not be defined under certain circumstances.
   */
  class S3fsSeekableCachingEntityBody extends Guzzle\Http\CachingEntityBody {

    /**
     * This version of seek() allows seeking past the end of the cache.
     *
     * If the caller attempts to seek more than 50 megs into the file,
     * though, an exception will be thrown, because that would take up too
     * much memory.
     */
    public function seek($offset, $whence = SEEK_SET) {
      if ($whence == SEEK_SET) {
        $byte = $offset;
      }
      else if ($whence == SEEK_CUR) {
        $byte = $offset + $this->ftell();
      }
      else {
        throw new RuntimeException(__CLASS__ . ' supports only SEEK_SET and SEEK_CUR seek operations');
      }

      if ($byte > 52428800) {
        throw new RuntimeException(
          "Seeking more than 50 megabytes into a remote file is not supported, due to memory constraints.
            If you need to bypass this error, please contact the maintainers of S3 File System."
        );
      }

      // If the caller tries to seek past the end of the currently cached
      // data, read in enough of the remote stream to let the seek occur.
      while ($byte > $this->body->getSize() && !$this->isConsumed()) {
        $this->read(16384);
      }

      return $this->body->seek($byte);
    }
  }
}
