<?php

/**
 * @file
 * Defines a drush command that refreshes the S3 metadata cache.
 */

/**
 * Implements hook_drush_command().
 */
function s3fs_drush_command() {
  $items = array();
  $items['s3fs-refresh-cache'] = array(
    'description' => t('Refresh the S3 File System metadata cache.'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_FULL,
    'aliases' => array('s3fs-rc'),
    'callback' => 'drush_s3fs_refresh_cache',
  );
  $items['s3fs-copy-local'] = array(
    'description' => t('Copy local files from your public and/or private file system(s) into your S3 bucket, if s3fs is configured to take them over.'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_FULL,
    'aliases' => array('s3fs-cl'),
    'callback' => 'drush_s3fs_copy_local',
  );
  return $items;
}

/**
 * Refreshes the file metadata cache.
 */
function drush_s3fs_refresh_cache() {
  $config = _s3fs_get_config();
  if (!_s3fs_validate_config($config)) {
    drupal_set_message(
      t('Unable to validate your s3fs configuration settings. Please configure S3 File System from the admin/config/media/s3fs page and try again.'),
      'error'
    );
    return;
  }
  _s3fs_refresh_cache($config);
}

/**
 * Copys all files from the local public/private filesystem folders into S3,
 * if s3fs is configured to take over those systems.
 */
function drush_s3fs_copy_local() {
  $config = _s3fs_get_config();
  if (!_s3fs_validate_config($config)) {
    drupal_set_message(
      t('Unable to validate your s3fs configuration settings. Please configure S3 File System from the admin/config/media/s3fs page and try again.'),
      'error'
    );
    return;
  }
  if (empty($config['use_s3_for_public']) && empty($config['use_s3_for_private'])) {
    drupal_set_message(
      t('S3 File System is not configured to take over any other file systems.')
    );
    return;
  }
  if (!empty($config['use_s3_for_public'])) {
    _s3fs_copy_file_system_to_s3('public');
  }
  if (!empty($config['use_s3_for_private'])) {
    _s3fs_copy_file_system_to_s3('private');
  }
}
